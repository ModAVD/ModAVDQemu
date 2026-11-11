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
#include "android/emulator-safe-shutdown.h"

#include "aemu/base/async/ThreadLooper.h"
#include "aemu/base/threads/Async.h"
#include "android/base/system/System.h"
#include "android/console.h"
#include "android/emulation/AdbHostServer.h"
#include "host-common/vm_operations.h"

namespace android {

using base::System;

// A paused VM (console "avd pause", gRPC setVmState) runs no vCPU, so the
// guest's adbd does not answer: the sync and shutdown commands would wait out
// their limits and the emulator would quit without a sync. Resume it first, on
// the QEMU main loop as gRPC does: the transition takes the I/O lock. Only a
// plain pause is undone; a debugger stop, a snapshot save or load and an I/O
// error are left as they are.
static bool resumePausedVm() {
    const auto* vm = getConsoleAgents()->vm;
    if (!vm || vm->getRunState() != QEMU_RUN_STATE_PAUSED) {
        return false;
    }
    base::ThreadLooper::runOnMainLooper([vm] { vm->vmResume(); });
    return true;
}

// The private adb server may have been restarted since this instance
// registered with it at boot (adb kill-server, a crash). With local transport
// scanning off, as the manager and AUTO-MAS run it, the server then does not
// know the instance until the guest's adbd reconnects, and the shutdown and
// sync commands would fail with "device not found". Register again, the way
// the instance does at boot. A server that is not running yet is started by
// the first adb command, so keep trying for a few seconds.
static void registerWithAdbServer() {
    const int adbPort = getConsoleAgents()->settings->android_adb_port();
    const int clientPort = emulation::AdbHostServer::getClientPort();
    if (adbPort <= 0 || clientPort <= 0) {
        return;
    }
    base::async([adbPort, clientPort] {
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (emulation::AdbHostServer::notify(adbPort, clientPort)) {
                return;
            }
            System::get()->sleepMs(1000);
        }
    });
}

SafeShutdown::Actions emulatorSafeShutdownActions() {
    SafeShutdown::Actions actions;
    actions.killSelfAfter = [](int seconds) {
        System::get()->waitAndKillSelf(seconds);
    };
    actions.resumePausedVm = &resumePausedVm;
    actions.registerWithAdbServer = &registerWithAdbServer;
    return actions;
}

}  // namespace android
