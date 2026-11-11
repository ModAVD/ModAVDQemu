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

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "android/emulation/control/adb/AdbInterface.h"

using android::SafeShutdown;
using android::emulation::AdbCommand;
using android::emulation::AdbCommandPtr;
using android::emulation::AdbInterface;
using android::emulation::OptionalAdbCommandResult;
using State = android::SafeShutdown::State;

namespace {

class FakeCommand : public AdbCommand {
public:
    bool inFlight() const override { return true; }
    void cancel() override {}
    void start(int) override {}
};

// Records the commands; the test completes them when and how it likes.
class FakeAdb : public AdbInterface {
public:
    struct Command {
        std::vector<std::string> args;
        ResultCallback callback;
        android::base::System::Duration timeoutMs;
    };

    bool isAdbVersionCurrent() const override { return true; }
    void setCustomAdbPath(const std::string&) override {}
    const std::string& detectedAdbPath() const override { return mEmpty; }
    const std::string& adbPath() override { return mEmpty; }
    void setSerialNumberPort(int) override {}
    const std::string& serialString() const override { return mEmpty; }
    void enqueueCommand(const std::vector<std::string>&,
                        ResultCallback) override {}

    AdbCommandPtr runAdbCommand(const std::vector<std::string>& args,
                                ResultCallback callback,
                                android::base::System::Duration timeoutMs,
                                bool) override {
        if (failToStart > 0) {
            --failToStart;
            return nullptr;
        }
        commands.push_back({args, std::move(callback), timeoutMs});
        return std::make_shared<FakeCommand>();
    }

    // The command ran: it exited with |exitCode| after printing |output|.
    void complete(size_t index, int exitCode, const std::string& output = "") {
        OptionalAdbCommandResult result;
        result.emplace(exitCode, new std::istringstream(output));
        commands.at(index).callback(result);
    }

    // The command did not finish in its time.
    void timeOut(size_t index) {
        commands.at(index).callback(OptionalAdbCommandResult());
    }

    std::string line(size_t index) const {
        std::string joined;
        for (const std::string& arg : commands.at(index).args) {
            joined += (joined.empty() ? "" : " ") + arg;
        }
        return joined;
    }

    std::vector<Command> commands;
    int failToStart = 0;

private:
    std::string mEmpty;
};

class FakeHost : public SafeShutdown::Host {
public:
    void post(std::function<void()> task) override {
        tasks.push_back(std::move(task));
    }
    void startPowerOffTimer(int ms) override { timerMs = ms; }
    void stopPowerOffTimer() override { timerMs = 0; }
    void stateChanged() override { ++stateChanges; }
    void quit() override { ++quits; }

    // The owner's thread gets to its queue.
    void runTasks() {
        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.pop_front();
            task();
        }
    }

    std::deque<std::function<void()>> tasks;
    int timerMs = 0;
    int stateChanges = 0;
    int quits = 0;
};

constexpr char kSync[] = "wait-for-device shell sync";
constexpr char kPowerOff[] = "wait-for-device shell svc power shutdown";
constexpr char kInitPowerOff[] =
        "wait-for-device shell su 0 setprop sys.powerctl shutdown";
const std::chrono::milliseconds kNoWait(0);

class SafeShutdownTest : public ::testing::Test {
protected:
    std::unique_ptr<SafeShutdown> make(bool withAdb = true) {
        SafeShutdown::Actions actions;
        actions.killSelfAfter = [this](int seconds) {
            guards.push_back(seconds);
        };
        actions.resumePausedVm = [this] {
            ++resumes;
            return vmPaused;
        };
        actions.registerWithAdbServer = [this] { ++registrations; };
        return std::make_unique<SafeShutdown>(
                "Test shutdown", &host,
                [this, withAdb]() -> AdbInterface* {
                    return withAdb ? &adb : nullptr;
                },
                std::move(actions));
    }

    FakeAdb adb;
    FakeHost host;
    std::vector<int> guards;
    int resumes = 0;
    int registrations = 0;
    bool vmPaused = false;
};

}  // namespace

TEST_F(SafeShutdownTest, SyncsThenAsksAndroidToPowerOff) {
    auto shutdown = make();
    EXPECT_FALSE(shutdown->inProgress());
    shutdown->start();

    EXPECT_EQ(shutdown->state(), State::StoppingGuest);
    EXPECT_TRUE(shutdown->inProgress());
    EXPECT_EQ(guards, std::vector<int>{SafeShutdown::kGuardSec});
    EXPECT_EQ(host.timerMs, SafeShutdown::kPowerOffMs);
    EXPECT_EQ(host.stateChanges, 1);
    EXPECT_EQ(resumes, 1);
    EXPECT_EQ(registrations, 1);
    ASSERT_EQ(adb.commands.size(), 1u);
    EXPECT_EQ(adb.line(0), kSync);
    EXPECT_EQ(adb.commands[0].timeoutMs, 5000);

    // Nothing is asked of Android before the sync is back, and then only on
    // the owner's thread.
    adb.complete(0, 0);
    EXPECT_EQ(adb.commands.size(), 1u);
    host.runTasks();
    ASSERT_EQ(adb.commands.size(), 2u);
    EXPECT_EQ(adb.line(1), kPowerOff);
    EXPECT_EQ(adb.commands[1].timeoutMs, SafeShutdown::kPowerOffMs);

    // Android took it: wait for it to power off, with the timer still on.
    adb.complete(1, 0);
    host.runTasks();
    EXPECT_EQ(adb.commands.size(), 2u);
    EXPECT_EQ(shutdown->state(), State::StoppingGuest);
    EXPECT_EQ(host.timerMs, SafeShutdown::kPowerOffMs);
    EXPECT_EQ(host.quits, 0);

    // The emulator stops by itself once Android has powered off.
    shutdown->stopped();
    EXPECT_EQ(shutdown->state(), State::Idle);
    EXPECT_EQ(host.timerMs, 0);
    EXPECT_EQ(host.quits, 0);
}

TEST_F(SafeShutdownTest, StartIsOnce) {
    auto shutdown = make();
    shutdown->start();
    shutdown->start();
    EXPECT_EQ(adb.commands.size(), 1u);
    EXPECT_EQ(guards.size(), 1u);
}

TEST_F(SafeShutdownTest, AsksAndroidEvenIfTheSyncFailed) {
    auto shutdown = make();
    shutdown->start();
    adb.complete(0, 1);
    host.runTasks();
    ASSERT_EQ(adb.commands.size(), 2u);
    EXPECT_EQ(adb.line(1), kPowerOff);

    // And if it timed out.
    adb.commands.clear();
    auto second = make();
    second->start();
    adb.timeOut(0);
    host.runTasks();
    ASSERT_EQ(adb.commands.size(), 2u);
    EXPECT_EQ(adb.line(1), kPowerOff);
}

TEST_F(SafeShutdownTest, AsksAndroidWhenTheSyncCannotStart) {
    auto shutdown = make();
    adb.failToStart = 1;
    shutdown->start();
    ASSERT_EQ(adb.commands.size(), 1u);
    EXPECT_EQ(adb.line(0), kPowerOff);
    // The sync that never ran is not waited for.
    EXPECT_TRUE(shutdown->waitForSyncs(kNoWait));
}

TEST_F(SafeShutdownTest, InitPowersOffWhenAndroidRefuses) {
    auto shutdown = make();
    shutdown->start();
    adb.complete(0, 0);
    host.runTasks();
    // svc prints why and still exits with 0.
    adb.complete(1, 0, "\n  Error: not allowed \r\n");
    host.runTasks();
    ASSERT_EQ(adb.commands.size(), 3u);
    EXPECT_EQ(adb.line(2), kInitPowerOff);
    EXPECT_EQ(adb.commands[2].timeoutMs, 15000);

    adb.complete(2, 0);
    host.runTasks();
    EXPECT_EQ(adb.commands.size(), 3u);
    EXPECT_EQ(shutdown->state(), State::StoppingGuest);
    EXPECT_EQ(host.quits, 0);
}

TEST_F(SafeShutdownTest, SyncsAndQuitsWhenInitRefusesToo) {
    auto shutdown = make();
    shutdown->start();
    adb.complete(0, 0);
    host.runTasks();
    adb.complete(1, 0, "Error");
    host.runTasks();
    adb.complete(2, 1);
    host.runTasks();

    EXPECT_EQ(shutdown->state(), State::Syncing);
    EXPECT_EQ(host.timerMs, 0);
    ASSERT_EQ(adb.commands.size(), 4u);
    EXPECT_EQ(adb.line(3), kSync);
    EXPECT_EQ(host.quits, 0);

    adb.complete(3, 0);
    EXPECT_EQ(host.quits, 0);
    host.runTasks();
    EXPECT_EQ(shutdown->state(), State::Forced);
    EXPECT_EQ(host.quits, 1);
    EXPECT_EQ(guards, (std::vector<int>{SafeShutdown::kGuardSec, 10}));
    EXPECT_EQ(host.stateChanges, 2);
    EXPECT_TRUE(shutdown->inProgress());
}

TEST_F(SafeShutdownTest, SyncsAndQuitsWhenAndroidDoesNotPowerOffInTime) {
    auto shutdown = make();
    shutdown->start();
    adb.complete(0, 0);
    host.runTasks();
    ASSERT_EQ(adb.commands.size(), 2u);

    shutdown->powerOffTimedOut();
    EXPECT_EQ(shutdown->state(), State::Syncing);
    ASSERT_EQ(adb.commands.size(), 3u);
    EXPECT_EQ(adb.line(2), kSync);
    EXPECT_EQ(registrations, 2);

    // The shutdown command runs out of time at about the same moment: it has
    // nothing more to say.
    adb.timeOut(1);
    host.runTasks();
    EXPECT_EQ(adb.commands.size(), 3u);
    EXPECT_EQ(shutdown->state(), State::Syncing);

    // Quit even if this last sync failed; either way it is over.
    EXPECT_FALSE(shutdown->waitForSyncs(kNoWait));
    adb.complete(2, 1);
    EXPECT_TRUE(shutdown->waitForSyncs(kNoWait));
    host.runTasks();
    EXPECT_EQ(shutdown->state(), State::Forced);
    EXPECT_EQ(host.quits, 1);

    // The timer has nothing to do any more.
    shutdown->powerOffTimedOut();
    EXPECT_EQ(adb.commands.size(), 3u);
    EXPECT_EQ(host.quits, 1);
}

TEST_F(SafeShutdownTest, TimeoutOnlyCountsWhileAndroidIsShuttingDown) {
    auto shutdown = make();
    shutdown->powerOffTimedOut();
    EXPECT_EQ(shutdown->state(), State::Idle);
    EXPECT_TRUE(adb.commands.empty());
}

TEST_F(SafeShutdownTest, QuitsAtOnceWhenTheLastSyncCannotStart) {
    auto shutdown = make();
    shutdown->start();
    adb.complete(0, 0);
    host.runTasks();
    adb.failToStart = 1;
    shutdown->powerOffTimedOut();
    EXPECT_EQ(shutdown->state(), State::Forced);
    EXPECT_EQ(host.quits, 1);
    EXPECT_TRUE(shutdown->waitForSyncs(kNoWait));
}

TEST_F(SafeShutdownTest, QuitsWithoutAdb) {
    auto shutdown = make(/*withAdb=*/false);
    shutdown->start();
    EXPECT_EQ(shutdown->state(), State::Forced);
    EXPECT_EQ(host.quits, 1);
    EXPECT_EQ(host.timerMs, 0);
    EXPECT_TRUE(adb.commands.empty());
    EXPECT_EQ(guards, (std::vector<int>{SafeShutdown::kGuardSec, 10}));
}

TEST_F(SafeShutdownTest, ForceQuitIsOnce) {
    auto shutdown = make();
    shutdown->start();
    shutdown->forceQuit();
    shutdown->forceQuit();
    EXPECT_EQ(host.quits, 1);
    EXPECT_EQ(shutdown->state(), State::Forced);
    EXPECT_EQ(host.timerMs, 0);

    // What comes back from the guest afterwards changes nothing.
    adb.complete(0, 0);
    host.runTasks();
    EXPECT_EQ(adb.commands.size(), 1u);
    EXPECT_EQ(host.quits, 1);
}

TEST_F(SafeShutdownTest, ResumesAPausedVm) {
    vmPaused = true;
    auto shutdown = make();
    shutdown->start();
    EXPECT_EQ(resumes, 1);
    EXPECT_EQ(adb.commands.size(), 1u);
}

TEST_F(SafeShutdownTest, WaitsForTheSyncInFlight) {
    auto shutdown = make();
    EXPECT_TRUE(shutdown->waitForSyncs(kNoWait));
    shutdown->start();
    EXPECT_FALSE(shutdown->waitForSyncs(std::chrono::milliseconds(1)));

    // With the first sync still running there is no call for another.
    shutdown->syncForSessionEnd();
    EXPECT_EQ(adb.commands.size(), 1u);

    // The wait is over as soon as the command is back, before the owner's
    // thread, which is the one waiting, gets to anything.
    adb.complete(0, 0);
    EXPECT_TRUE(shutdown->waitForSyncs(kNoWait));
}

TEST_F(SafeShutdownTest, SyncsOnceMoreAsTheSessionEnds) {
    auto shutdown = make();
    // Not before the shutdown has started.
    shutdown->syncForSessionEnd();
    EXPECT_TRUE(adb.commands.empty());

    shutdown->start();
    adb.complete(0, 0);
    host.runTasks();
    ASSERT_EQ(adb.commands.size(), 2u);

    shutdown->syncForSessionEnd();
    ASSERT_EQ(adb.commands.size(), 3u);
    // Without wait-for-device: adbd may be gone by now.
    EXPECT_EQ(adb.line(2), "shell sync");
    EXPECT_EQ(adb.commands[2].timeoutMs, 3000);
    EXPECT_FALSE(shutdown->waitForSyncs(kNoWait));

    adb.complete(2, 0);
    EXPECT_TRUE(shutdown->waitForSyncs(kNoWait));
    EXPECT_EQ(shutdown->state(), State::StoppingGuest);
}

TEST_F(SafeShutdownTest, NoExtraSessionEndSyncDuringTheLastSync) {
    auto shutdown = make();
    shutdown->start();
    adb.complete(0, 0);
    host.runTasks();
    shutdown->powerOffTimedOut();
    ASSERT_EQ(adb.commands.size(), 3u);
    shutdown->syncForSessionEnd();
    EXPECT_EQ(adb.commands.size(), 3u);
    EXPECT_FALSE(shutdown->waitForSyncs(kNoWait));
}

TEST_F(SafeShutdownTest, CommandsMayCompleteAfterTheShutdownIsGone) {
    auto shutdown = make();
    shutdown->start();
    shutdown->detach();
    // Nothing reaches the host any more, and the sync is still counted.
    EXPECT_FALSE(shutdown->waitForSyncs(kNoWait));
    adb.complete(0, 0);
    EXPECT_TRUE(host.tasks.empty());
    EXPECT_TRUE(shutdown->waitForSyncs(kNoWait));

    // A task already handed to the owner's thread finds the shutdown gone.
    auto second = make();
    adb.commands.clear();
    second->start();
    adb.complete(0, 0);
    ASSERT_EQ(host.tasks.size(), 1u);
    second.reset();
    host.runTasks();
    EXPECT_EQ(adb.commands.size(), 1u);

    // And a command that completes later still.
    auto third = make();
    adb.commands.clear();
    third->start();
    third.reset();
    adb.complete(0, 0);
    EXPECT_TRUE(host.tasks.empty());
}
