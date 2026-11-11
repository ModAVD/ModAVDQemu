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

#include "android/safe-shutdown.h"

namespace android {

// What the safe shutdown does to this emulator's process and VM, with or
// without a window: the kill guard, resuming a paused VM on the QEMU main
// loop, registering with the adb server again.
SafeShutdown::Actions emulatorSafeShutdownActions();

// The headless emulator on Windows (ModAVD#64). It has no window, so Windows
// does not tell it that the session is ending (logoff, restart, shutdown) and
// just ends the process: for the guest the power is cut. This makes a hidden
// top-level window on a thread of its own, which gets WM_QUERYENDSESSION and
// WM_ENDSESSION and runs the safe shutdown on them: resume a paused VM, sync,
// have Android shut down. A program without a visible window cannot hold the
// session's end up, so as it ends the thread only waits a few seconds for the
// guest sync: Android may not get to power off, but what was written is on
// disk.
//
// |adb| is asked for the interface when the session ends. Does nothing but on
// Windows, and nothing with the full toolbar (ANDROID_EMU_FULL_TOOLBAR=1),
// which keeps the emulator as it comes.
void startHeadlessSessionEndWatch(
        std::function<emulation::AdbInterface*()> adb);

}  // namespace android
