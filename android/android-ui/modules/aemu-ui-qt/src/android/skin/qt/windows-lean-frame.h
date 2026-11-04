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

#include <qwindowdefs.h>

#include <functional>

#include "android/lean-window-frame.h"

// The Win32 side of the lean window's frame (ModAVD#58, see
// android/lean-window-frame.h). Qt sees a frameless window. Windows sees a
// window with the system caption style, but its client area covers the whole
// window: the caption style brings back the DWM shadow and border, and the
// title bar the emulator draws (LeanTitleBar) sits at the top of the client
// area. There is no resizable frame and no maximize box, and the window is
// moved and resized here, never in the system's move and size loops, so
// Windows never snaps, docks or maximizes it.
class WindowsLeanFrame {
public:
    struct Geometry {
        // Device pixels per logical pixel.
        double scale = 1.0;
        // The title bar's height and where its buttons start, in device
        // pixels from the window's top-left corner.
        int titleHeight = 0;
        int buttonsLeft = 0;
        // The guest screen's width / height; 0 while it is not known.
        double guestAspect = 0;
        // The smallest guest screen, in device pixels.
        int minGuestWidth = 1;
        int minGuestHeight = 1;
    };

    // |resizeDone| gets the window's size in device pixels when a resize by
    // an edge or a corner ends.
    WindowsLeanFrame(std::function<Geometry()> geometry,
                     std::function<void(int width, int height)> resizeDone);

    // Gives the native window |wid| its frame: the caption style without the
    // resizable frame and the maximize box, the frame extended into the
    // client area for the shadow, rounded corners on Windows 11, the dark
    // border, and a system menu with only minimize and close. Safe to repeat,
    // and repeated whenever Qt may have reset the window style.
    static void apply(WId wid);

    // Shows the window's system menu at the screen point (x, y) and carries
    // out the command picked.
    static void showSystemMenu(WId wid, int x, int y);

    // Handles a message (MSG*) of the window. Returns true, with the window
    // procedure's |result|, when it handled it.
    bool handleMessage(void* message, qintptr* result);

    bool tracking() const { return mMode != Mode::None; }

private:
    enum class Mode { None, Move, Resize };

    void startTracking(void* hwnd, android::lean_window::Hit hit, int x, int y);
    void track(void* hwnd, int x, int y);
    void stopTracking(void* hwnd, bool releaseCapture);

    std::function<Geometry()> mGeometry;
    std::function<void(int width, int height)> mResizeDone;
    Mode mMode = Mode::None;
    android::lean_window::Hit mHit = android::lean_window::Hit::Nowhere;
    android::lean_window::Rect mStartRect = {0, 0, 0, 0};
    android::lean_window::Point mGrab = {0, 0};
    double mStartScale = 1.0;
};
