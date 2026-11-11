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
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace android {

namespace emulation {
class AdbInterface;
}  // namespace emulation

// The safe shutdown: sync the guest, have Android shut itself down the way
// its power menu does, and quit when it has powered off; if it does not in
// time, sync once more and quit anyway. The lean window runs it when it is
// closed and when Windows ends the session (ModAVD#32, ModAVD#60), a headless
// instance when Windows ends the session (ModAVD#64).
//
// It knows nothing of windows: what it needs from the program around it comes
// through Host and Actions. Everything here runs on one thread, the owner's
// (the Qt UI thread, the headless message loop); the adb commands complete on
// the AdbInterface's thread and are handed back through Host::post().
class SafeShutdown {
public:
    // How long Android gets to power off. Its own shutdown may wait up to 10 s
    // for the shutdown broadcast and 10 s for the activities to stop before
    // init stops the services and unmounts; it usually takes 4-7 s in all.
    // Then comes a last sync (up to 5 s) and the quit; the guard kills the
    // process if even that does not finish.
    static constexpr int kPowerOffMs = 25000;
    static constexpr int kGuardSec = 35;

    enum class State {
        Idle,
        // The first sync, then Android shutting down.
        StoppingGuest,
        // Android did not power off: the last sync before the quit.
        Syncing,
        // Quitting without waiting for Android any longer.
        Forced,
    };

    class Host {
    public:
        virtual ~Host() = default;
        // Runs |task| on the owner's thread, later. The only one called from
        // other threads.
        virtual void post(std::function<void()> task) = 0;
        // Has powerOffTimedOut() called after |ms|, unless
        // stopPowerOffTimer() comes first.
        virtual void startPowerOffTimer(int ms) = 0;
        virtual void stopPowerOffTimer() = 0;
        // The shutdown has started, or is now forced: for what shows it.
        virtual void stateChanged() {}
        // Quits the emulator the usual way: QEMU stops the VM and closes its
        // disks.
        virtual void quit() = 0;
    };

    // What the shutdown does to the process and the VM.
    struct Actions {
        // Kills the process if it is still there after |seconds|.
        std::function<void(int seconds)> killSelfAfter;
        // Resumes a paused VM, which would not answer adb. Returns whether it
        // was paused.
        std::function<bool()> resumePausedVm;
        // Makes the adb server know the instance again, in case the server
        // was restarted since boot.
        std::function<void()> registerWithAdbServer;
    };

    // |name| starts the log lines. |adb| is asked for the interface when the
    // shutdown starts; it may return null.
    SafeShutdown(std::string name,
                 Host* host,
                 std::function<emulation::AdbInterface*()> adb,
                 Actions actions);
    ~SafeShutdown();

    // From here on adb commands that complete no longer reach this object or
    // its host. For an owner whose destructor has more to do before this
    // member goes.
    void detach();

    State state() const { return mState; }
    // Forced counts too: a close request in the seconds before QEMU quits
    // must not start anything else.
    bool inProgress() const { return mState != State::Idle; }

    void start();
    // The host's timer: Android has not powered off in time.
    void powerOffTimedOut();
    // Quits without waiting for Android to power off.
    void forceQuit();
    // The emulator has stopped, as it does when Android has powered off:
    // there is nothing left to wait for.
    void stopped();

    // Windows is ending the session with the shutdown under way, and ends the
    // process soon. If the first sync has finished, or failed, Android may
    // have written more since: sync once more.
    void syncForSessionEnd();
    // Waits for the guest syncs that have not completed. Returns whether none
    // is left. Their callbacks run on the AdbInterface's thread, so waiting
    // on the owner's thread does not hold them up.
    bool waitForSyncs(std::chrono::milliseconds timeout);

private:
    struct Shared;

    void requestGuestPowerOff();
    void requestInitPowerOff();
    void startSync();

    const std::string mName;
    Host* const mHost;
    const std::function<emulation::AdbInterface*()> mAdb;
    const Actions mActions;
    State mState = State::Idle;
    // What the adb callbacks hold on to: they may complete after this object
    // is gone.
    const std::shared_ptr<Shared> mShared;
};

}  // namespace android
