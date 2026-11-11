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

#ifdef _WIN32

#include <windows.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "android/avd/info.h"
#include "android/base/system/System.h"
#include "android/console.h"
#include "android/skin/winsys.h"
#include "android/utils/debug.h"

namespace android {

namespace {

constexpr char kName[] = "Safe headless shutdown";
constexpr wchar_t kWindowClass[] = L"ModAVDHeadlessSessionEnd";
// Tasks are waiting in the queue (Host::post).
constexpr UINT kTasksMessage = WM_APP + 1;
constexpr UINT_PTR kPowerOffTimer = 1;

// The hidden window, its thread, and the safe shutdown that runs on it. One
// per process, never destroyed: the thread ends with the process.
class SessionEndWatch final : public SafeShutdown::Host {
public:
    explicit SessionEndWatch(std::function<emulation::AdbInterface*()> adb)
        : mShutdown(kName,
                    this,
                    std::move(adb),
                    emulatorSafeShutdownActions()) {}

    // The thread: makes the window and runs its messages.
    void run() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &SessionEndWatch::windowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&windowClass)) {
            dwarning("%s: no window class (%lu); Windows ending the session will cut the guest's power",
                     kName, GetLastError());
            return;
        }
        // A top-level window, never shown. Not a message-only window: those
        // are not told about the session's end.
        const HWND window = CreateWindowExW(0, kWindowClass, L"ModAVD",
                                            WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                                            nullptr, instance, this);
        if (!window) {
            dwarning("%s: no window (%lu); Windows ending the session will cut the guest's power",
                     kName, GetLastError());
            return;
        }
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            DispatchMessageW(&message);
        }
    }

    // SafeShutdown::Host
    void post(std::function<void()> task) override {
        {
            std::lock_guard<std::mutex> lock(mTasksMutex);
            mTasks.push_back(std::move(task));
        }
        PostMessageW(mWindow.load(), kTasksMessage, 0, 0);
    }

    void startPowerOffTimer(int ms) override {
        SetTimer(mWindow.load(), kPowerOffTimer, static_cast<UINT>(ms), nullptr);
    }

    void stopPowerOffTimer() override {
        KillTimer(mWindow.load(), kPowerOffTimer);
    }

    // The headless emulator's own way out. By now the guest is synced, or
    // could not be.
    void quit() override { skin_winsys_quit_request(); }

private:
    static LRESULT CALLBACK windowProc(HWND window,
                                       UINT message,
                                       WPARAM wParam,
                                       LPARAM lParam) {
        if (message == WM_NCCREATE) {
            auto* self = static_cast<SessionEndWatch*>(
                    reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
            self->mWindow.store(window);
            return DefWindowProcW(window, message, wParam, lParam);
        }
        auto* self = reinterpret_cast<SessionEndWatch*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!self) {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        switch (message) {
            case WM_QUERYENDSESSION:
                // Windows is logging off, restarting or shutting down. Start
                // now, to give Android the time the other programs take; the
                // answer is yes either way, a program without a visible
                // window is not waited for.
                dinfo("%s: Windows is ending the session%s", kName,
                      (lParam & ENDSESSION_CRITICAL) ? " (critical)" : "");
                self->begin();
                return TRUE;

            case WM_ENDSESSION:
                self->endSession(wParam != FALSE);
                return 0;

            case WM_TIMER:
                if (wParam == kPowerOffTimer) {
                    KillTimer(window, kPowerOffTimer);  // one shot
                    self->mShutdown.powerOffTimedOut();
                }
                return 0;

            case WM_CLOSE:
                // The window stays for as long as the process: nothing closes
                // it (taskkill without /F would).
                return 0;

            default:
                if (message == kTasksMessage) {
                    self->runTasks();
                    return 0;
                }
                return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    void begin() {
        if (mShutdown.inProgress()) {
            return;
        }
        // A quick-boot snapshot of a guest that is shutting down is of no
        // use, so do not save one; the lean window does the same.
        getConsoleAgents()->settings->avdParams()->flags |=
                AVDINFO_NO_SNAPSHOT_SAVE_ON_EXIT;
        mShutdown.start();
    }

    // WM_ENDSESSION. When the session ends, Windows ends the process soon
    // after this returns: make sure the guest is syncing and give the sync a
    // few seconds. When it goes on after all (the user cancelled, another
    // program refused), Android is already powering off and cannot be called
    // back; the emulator quits when it has.
    void endSession(bool ending) {
        if (!ending) {
            if (mShutdown.inProgress()) {
                dinfo("%s: Windows is not ending the session after all", kName);
            }
            return;
        }
        if (!mShutdown.inProgress()) {
            dinfo("%s: Windows is ending the session without asking", kName);
            begin();
        } else {
            mShutdown.syncForSessionEnd();
        }
        if (mShutdown.waitForSyncs(std::chrono::seconds(3))) {
            dinfo("%s: Windows is ending the session; the guest is synced",
                  kName);
        } else {
            dwarning("%s: the guest sync has not finished as Windows ends the session",
                     kName);
        }
    }

    void runTasks() {
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lock(mTasksMutex);
                if (mTasks.empty()) {
                    return;
                }
                task = std::move(mTasks.front());
                mTasks.pop_front();
            }
            task();
        }
    }

    std::atomic<HWND> mWindow{nullptr};
    std::mutex mTasksMutex;
    std::deque<std::function<void()>> mTasks;
    SafeShutdown mShutdown;
};

}  // namespace

void startHeadlessSessionEndWatch(
        std::function<emulation::AdbInterface*()> adb) {
    if (base::System::get()->getEnvironmentVariable(
                "ANDROID_EMU_FULL_TOOLBAR") == "1") {
        return;
    }
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        return;
    }
    // Be told before programs at the default level (0x280): the private adb
    // server the sync and the shutdown go through keeps running until then.
    // The lean window does the same.
    SetProcessShutdownParameters(0x300, 0);
    auto* watch = new SessionEndWatch(std::move(adb));
    std::thread([watch] { watch->run(); }).detach();
}

}  // namespace android

#else  // !_WIN32

namespace android {

void startHeadlessSessionEndWatch(
        std::function<emulation::AdbInterface*()> adb) {
    (void)adb;
}

}  // namespace android

#endif  // _WIN32
