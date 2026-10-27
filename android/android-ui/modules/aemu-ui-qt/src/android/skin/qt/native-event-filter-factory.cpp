/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "android/skin/qt/native-event-filter-factory.h"

#include "aemu/base/memory/LazyInstance.h"         // for LazyInstance

class QAbstractNativeEventFilter;

#define DEBUG 1

#if DEBUG
#include "android/utils/debug.h"                      // for VERBOSE_PRINT

#define D(...) VERBOSE_PRINT(surface, __VA_ARGS__)
#else
#define D(...) ((void)0)
#endif

#ifdef __APPLE__

#import "android/skin/qt/mac-native-event-filter.h"

#elif defined(__linux__)
#include <cstdio>

#include "android/console.h"
#include "android/skin/keycode.h"
#include "android/skin/qt/emulator-qt-window.h"
#include "android/skin/qt/native-keyboard-event-handler.h"
#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <X11/XKBlib.h>
#include <xcb/xcb.h>

class NativeEventFilter : public QAbstractNativeEventFilter {
public:
    NativeEventFilter() : mNumLockMask(XCB_MOD_MASK_2) {
        // NumLock could be mapped to any one of MOD_MASK1 - MOD_MASK5
        // depending on system configurations. We should use virtual modifier to
        // to find which real modifier it is mapped to.
        Display* display = XOpenDisplay(NULL);

        if (!display) {
            dwarning("%s: warning: cannot get x11 display info", __func__);
            return;
        }

        XkbDescPtr xkb =
                XkbGetMap(display, XkbAllComponentsMask, XkbUseCoreKbd);

        if (!xkb) {
            dwarning("%s: warning: cannot get mod mask info", __func__);
            return;
        }

        if (XkbGetNames(display, XkbKeycodesNameMask, xkb) != 0) {
            dwarning("%s: warning: cannot get xkb keycodes names", __func__);
            return;
        }

        for (int i = 0; i < XkbNumVirtualMods; i++) {
            char* name = NULL;
            unsigned int realMod = 0;
            if (xkb->names->vmods[i]) {
                name = XGetAtomName(display, xkb->names->vmods[i]);
                if (!strcmp(name, "NumLock")) {
                    XkbVirtualModsToReal(xkb, 1 << i, &realMod);
                    mNumLockMask = realMod;
                    break;
                }
            }
        }
        XkbFreeClientMap(xkb, XkbAllComponentsMask, true);
        XCloseDisplay(display);
    }

    bool nativeEventFilter(const QByteArray& eventType,
                           void* message,
                           qintptr*) override {
        if (eventType.compare("xcb_generic_event_t") == 0) {
            xcb_generic_event_t* ev =
                    static_cast<xcb_generic_event_t*>(message);
            int evType = (ev->response_type & ~0x80);
            if (evType == XCB_KEY_PRESS) {
                xcb_key_press_event_t* keyEv = (xcb_key_press_event_t*)ev;
                if (getConsoleAgents()->settings->use_keycode_forwarding()) {
                    if (EmulatorQtWindow::getInstance()->isActiveWindow()) {
                        NativeKeyboardEventHandler::KeyEvent inputEv;
                        inputEv.scancode = keyEv->detail;
                        inputEv.modifiers = keyEv->state;
                        inputEv.eventType = kEventKeyDown;
                        std::optional<SkinEvent> maybeEvent =
                                NativeKeyboardEventHandler::getInstance()
                                        ->handleKeyEvent(inputEv);
                        if (maybeEvent.has_value())
                            EmulatorQtWindow::getInstance()->queueSkinEvent(
                                    std::move(maybeEvent.value()));
                    }
                }
            }
        }
        return false;
    }

private:
    unsigned int mNumLockMask;
};

#else  // windows

#include <windows.h>
#include "host-common/FeatureControl.h"
#include "android/skin/keycode.h"
#include "android/skin/qt/emulator-qt-window.h"
#include "android/skin/qt/native-keyboard-event-handler.h"
#include <QAbstractNativeEventFilter>
#include <QByteArray>

class NativeEventFilter : public QAbstractNativeEventFilter {
public:
    bool nativeEventFilter(const QByteArray& eventType,
                           void* message,
                           qintptr*) override {
        if (eventType.compare("windows_generic_MSG") == 0) {
            MSG* ev = static_cast<MSG*>(message);
            if (ev->message == WM_KEYDOWN || ev->message == WM_SYSKEYDOWN) {
                // Since being upgraded to 5.12.1, the prebuilt QT library
                // cannot handle numpad keys when number lock is on. To
                // workaround, we map numpad keys to its corresponding alpha
                // numerical keys when numlock is on.
                // TODO (wdu@) Find the root cause in prebuilt QT library.
                // Bug: 135141621
                UINT scancode = (ev->lParam >> 16) & 0x1ff;
                const UINT nativeScancode = scancode;
                // In host IME text mode, a key the input method took arrives
                // as VK_PROCESSKEY; leave it to the input method.
                EmulatorQtWindow* window = EmulatorQtWindow::getInstance();
                const bool imeTextMode = window && window->isHostImeTextMode();
                const bool imeKey = imeTextMode && ev->wParam == VK_PROCESSKEY;
                // Modifiers before the rename below; Qt reports no
                // auto-repeats of these.
                const bool modifier =
                        ev->wParam == VK_SHIFT || ev->wParam == VK_LSHIFT ||
                        ev->wParam == VK_RSHIFT || ev->wParam == VK_CONTROL ||
                        ev->wParam == VK_LCONTROL ||
                        ev->wParam == VK_RCONTROL || ev->wParam == VK_MENU ||
                        ev->wParam == VK_LMENU || ev->wParam == VK_RMENU ||
                        ev->wParam == VK_LWIN || ev->wParam == VK_RWIN ||
                        ev->wParam == VK_CAPITAL || ev->wParam == VK_NUMLOCK ||
                        ev->wParam == VK_SCROLL;
                if (LOWORD(GetKeyState(VK_NUMLOCK)) != 0 &&
                    skin_keycode_native_is_keypad(scancode) && !imeKey) {
                    scancode = skin_keycode_native_map_keypad(scancode);
                    ev->wParam = MapVirtualKeyA(scancode, MAPVK_VSC_TO_VK);
                }
                bool forwarded = false;
                bool deferred = false;
                if (getConsoleAgents()->settings->use_keycode_forwarding()) {
                    if (EmulatorQtWindow::getInstance()->isActiveWindow()) {
                        // With the host IME in play the key event decides
                        // whether the key still goes to the guest as it is;
                        // see EmulatorQtWindow::hostImeKeyPress().
                        deferred = window->hostImeDefersNativeKey(nativeScancode);
                        if (!deferred) {
                            NativeKeyboardEventHandler::KeyEvent keyEv;
                            keyEv.scancode = scancode;
                            keyEv.eventType = kEventKeyDown;
                            std::optional<SkinEvent> maybeEvent =
                                    NativeKeyboardEventHandler::getInstance()
                                            ->handleKeyEvent(keyEv);
                            if (maybeEvent.has_value()) {
                                EmulatorQtWindow::getInstance()
                                        ->queueSkinEvent(
                                                std::move(maybeEvent.value()));
                            }
                            forwarded = true;
                        }
                    }
                }
                // Only keys to the emulator window itself, not to the app's
                // other windows. Bit 30: the key was down before (an
                // auto-repeat).
                if (window && !imeKey &&
                    ev->hwnd == reinterpret_cast<HWND>(window->hostImeKeyWindow())) {
                    window->hostImeNativeKeyDown(
                            nativeScancode, (ev->lParam & (1 << 30)) == 0,
                            modifier, scancode, forwarded, deferred);
                }
            } else if (ev->message == WM_KEYUP || ev->message == WM_SYSKEYUP) {
                // A key up in any window is up. In text mode the input method
                // may take it (VK_PROCESSKEY), and Qt drops it; a key pressed
                // the usual way still has to be released in the guest.
                EmulatorQtWindow* window = EmulatorQtWindow::getInstance();
                if (window) {
                    window->hostImeNativeKeyUp(
                            (ev->lParam >> 16) & 0x1ff,
                            window->isHostImeTextMode() &&
                                    ev->wParam == VK_PROCESSKEY &&
                                    ev->hwnd == reinterpret_cast<HWND>(
                                                        window->hostImeKeyWindow()));
                }
            }
        }
        return false;
    }
};

#endif

static android::base::LazyInstance<NativeEventFilter> sInstance = LAZY_INSTANCE_INIT;
QAbstractNativeEventFilter* NativeEventFilterFactory::getEventFilter() {
    return sInstance.ptr();
}
