// Copyright 2026 qiyinxi
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "android/safe-shutdown.h"

#include <condition_variable>
#include <istream>
#include <mutex>
#include <utility>

#include "android/emulation/control/adb/AdbInterface.h"
#include "android/utils/debug.h"

namespace android {

using emulation::OptionalAdbCommandResult;

// The adb callbacks run on the AdbInterface's thread and can complete after
// the shutdown's owner is gone (the window is torn down once Android has
// powered off). They reach the shutdown only while |owner| is set, and count
// the syncs here whatever became of it.
struct SafeShutdown::Shared {
    std::mutex mutex;
    std::condition_variable syncsDone;
    int syncsInFlight = 0;
    SafeShutdown* owner = nullptr;

    void syncStarted() {
        std::lock_guard<std::mutex> lock(mutex);
        ++syncsInFlight;
    }

    void syncFinished() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            --syncsInFlight;
        }
        syncsDone.notify_all();
    }

    // Runs |task| with the shutdown, on its owner's thread, if it is still
    // there by then.
    static void post(const std::shared_ptr<Shared>& shared,
                     std::function<void(SafeShutdown&)> task) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (!shared->owner) {
            return;
        }
        shared->owner->mHost->post([shared, task = std::move(task)] {
            SafeShutdown* owner;
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                owner = shared->owner;
            }
            // detach() runs on this thread too, so the owner stays.
            if (owner) {
                task(*owner);
            }
        });
    }
};

namespace {

// The first line of what an adb command printed, trimmed; empty if it printed
// nothing.
std::string firstOutputLine(const OptionalAdbCommandResult& result) {
    if (!result || !result->output) {
        return {};
    }
    std::string line;
    while (std::getline(*result->output, line)) {
        const auto begin = line.find_first_not_of(" \t\r\n");
        if (begin != std::string::npos) {
            const auto end = line.find_last_not_of(" \t\r\n");
            return line.substr(begin, end - begin + 1);
        }
    }
    return {};
}

}  // namespace

SafeShutdown::SafeShutdown(std::string name,
                           Host* host,
                           std::function<emulation::AdbInterface*()> adb,
                           Actions actions)
    : mName(std::move(name)),
      mHost(host),
      mAdb(std::move(adb)),
      mActions(std::move(actions)),
      mShared(std::make_shared<Shared>()) {
    mShared->owner = this;
}

SafeShutdown::~SafeShutdown() {
    detach();
}

void SafeShutdown::detach() {
    std::lock_guard<std::mutex> lock(mShared->mutex);
    mShared->owner = nullptr;
}

void SafeShutdown::start() {
    if (mState != State::Idle) {
        return;
    }

    mState = State::StoppingGuest;
    dinfo("%s: syncing the guest, then asking Android to power off",
          mName.c_str());
    mActions.killSelfAfter(kGuardSec);
    mHost->stateChanged();
    mHost->startPowerOffTimer(kPowerOffMs);
    if (mActions.resumePausedVm()) {
        dinfo("%s: the VM is paused; resuming it first", mName.c_str());
    }
    auto* adb = mAdb();
    if (!adb) {
        dwarning("%s has no ADB interface; syncing guest", mName.c_str());
        startSync();
        return;
    }

    mActions.registerWithAdbServer();
    // Sync before Android starts shutting down: what was written before the
    // close is then on disk even if the process is killed while Android powers
    // off (Task Manager, Windows ending the session), and it no longer hangs
    // on the fallback sync after the timeout, when adbd may be gone.
    const auto shared = mShared;
    shared->syncStarted();
    auto command = adb->runAdbCommand(
            {"wait-for-device", "shell", "sync"},
            [shared](const OptionalAdbCommandResult& result) {
                shared->syncFinished();
                const bool success = result && result->exit_code == 0;
                Shared::post(shared, [success](SafeShutdown& self) {
                    if (self.mState != State::StoppingGuest) {
                        return;
                    }
                    if (success) {
                        dinfo("%s: guest synced; asking Android to power off",
                              self.mName.c_str());
                    } else {
                        dwarning("%s guest sync failed; asking Android to power off anyway",
                                 self.mName.c_str());
                    }
                    self.requestGuestPowerOff();
                });
            },
            5000);
    if (!command) {
        shared->syncFinished();
        dwarning("%s guest sync could not start; asking Android to power off anyway",
                 mName.c_str());
        requestGuestPowerOff();
    }
}

// Android's own shutdown, as its power menu runs it: apps get the shutdown
// broadcast and the foreground activity is paused and stopped before init
// stops everything. Setting sys.powerctl would go to init directly, which
// kills the apps wherever they are, perhaps halfway through rewriting a file.
// It needs no root. The command does not return: adbd goes away as Android
// shuts down, and adb then fails without printing anything. When svc refuses,
// it prints why (and still exits with 0); then init does it.
void SafeShutdown::requestGuestPowerOff() {
    auto* adb = mAdb();
    if (!adb) {
        dwarning("%s command could not start; syncing guest", mName.c_str());
        startSync();
        return;
    }
    const auto shared = mShared;
    auto command = adb->runAdbCommand(
            {"wait-for-device", "shell", "svc", "power", "shutdown"},
            [shared](const OptionalAdbCommandResult& result) {
                const bool timedOut = !result;
                const std::string output = firstOutputLine(result);
                Shared::post(shared, [timedOut, output](SafeShutdown& self) {
                    if (self.mState != State::StoppingGuest) {
                        return;
                    }
                    if (timedOut) {
                        dinfo("%s: Android is still shutting down",
                              self.mName.c_str());
                        return;
                    }
                    if (!output.empty()) {
                        dwarning("%s: Android did not take the shutdown request ('%s'); asking init to power off",
                                 self.mName.c_str(), output.c_str());
                        self.requestInitPowerOff();
                        return;
                    }
                    dinfo("Guest shutdown command accepted; waiting for Android to power off");
                });
            },
            kPowerOffMs);
    if (!command) {
        dwarning("%s command could not start; syncing guest", mName.c_str());
        startSync();
    }
}

// sys.powerctl is root-only; these userdebug images allow `su 0`.
void SafeShutdown::requestInitPowerOff() {
    auto* adb = mAdb();
    if (!adb) {
        dwarning("%s command could not start; syncing guest", mName.c_str());
        startSync();
        return;
    }
    const auto shared = mShared;
    auto command = adb->runAdbCommand(
            {"wait-for-device", "shell", "su", "0", "setprop", "sys.powerctl",
             "shutdown"},
            [shared](const OptionalAdbCommandResult& result) {
                const bool success = result && result->exit_code == 0;
                Shared::post(shared, [success](SafeShutdown& self) {
                    if (self.mState != State::StoppingGuest) {
                        return;
                    }
                    if (!success) {
                        dwarning("Guest shutdown command failed; syncing guest");
                        self.startSync();
                        return;
                    }

                    dinfo("Guest shutdown command accepted; waiting for Android to power off");
                });
            },
            15000);
    if (!command) {
        dwarning("%s command could not start; syncing guest", mName.c_str());
        startSync();
    }
}

void SafeShutdown::startSync() {
    if (mState == State::Syncing || mState == State::Forced) {
        return;
    }

    mState = State::Syncing;
    mHost->stopPowerOffTimer();
    auto* adb = mAdb();
    if (!adb) {
        dwarning("%s could not run guest sync", mName.c_str());
        forceQuit();
        return;
    }

    mActions.registerWithAdbServer();
    const auto shared = mShared;
    shared->syncStarted();
    auto command = adb->runAdbCommand(
            {"wait-for-device", "shell", "sync"},
            [shared](const OptionalAdbCommandResult& result) {
                shared->syncFinished();
                const bool success = result && result->exit_code == 0;
                Shared::post(shared, [success](SafeShutdown& self) {
                    if (self.mState != State::Syncing) {
                        return;
                    }
                    if (success) {
                        dinfo("%s: guest synced; quitting", self.mName.c_str());
                    } else {
                        dwarning("%s guest sync failed; forcing exit",
                                 self.mName.c_str());
                    }
                    self.forceQuit();
                });
            },
            // The guard is still far off: give a sync with a lot of dirty
            // data time to finish.
            5000);
    if (!command) {
        shared->syncFinished();
        dwarning("%s guest sync could not start; forcing exit", mName.c_str());
        forceQuit();
    }
}

void SafeShutdown::powerOffTimedOut() {
    if (mState == State::StoppingGuest) {
        dwarning("%s timed out; syncing guest", mName.c_str());
        startSync();
    }
}

void SafeShutdown::forceQuit() {
    if (mState == State::Forced) {
        return;
    }
    mState = State::Forced;
    dwarning("%s: quitting without waiting for Android to power off",
             mName.c_str());
    mHost->stateChanged();
    mHost->stopPowerOffTimer();
    mHost->quit();
    // Long enough for QEMU to stop the VM and close its disks after the quit.
    mActions.killSelfAfter(10);
}

void SafeShutdown::stopped() {
    mHost->stopPowerOffTimer();
    mState = State::Idle;
}

// Without wait-for-device: late in Android's shutdown adbd may be gone, and
// then this fails at once rather than use up the wait that follows.
void SafeShutdown::syncForSessionEnd() {
    if (mState != State::StoppingGuest) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mShared->mutex);
        if (mShared->syncsInFlight > 0) {
            return;
        }
    }
    auto* adb = mAdb();
    if (!adb) {
        return;
    }
    dinfo("%s: syncing the guest once more as Windows ends the session",
          mName.c_str());
    const auto shared = mShared;
    const std::string name = mName;
    shared->syncStarted();
    auto command = adb->runAdbCommand(
            {"shell", "sync"},
            [shared, name](const OptionalAdbCommandResult& result) {
                shared->syncFinished();
                if (!result || result->exit_code != 0) {
                    dwarning("%s: the last guest sync failed", name.c_str());
                }
            },
            3000);
    if (!command) {
        shared->syncFinished();
    }
}

bool SafeShutdown::waitForSyncs(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mShared->mutex);
    return mShared->syncsDone.wait_for(
            lock, timeout, [this] { return mShared->syncsInFlight == 0; });
}

}  // namespace android
