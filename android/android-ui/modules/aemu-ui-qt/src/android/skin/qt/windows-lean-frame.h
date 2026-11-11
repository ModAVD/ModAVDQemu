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
// window with the system's caption, resizable frame and maximize box, but its
// client area covers the whole window: the styles bring the DWM shadow and
// border, and let the system move, resize, maximize and snap the window
// (ModAVD#161). The title bar the emulator draws (LeanTitleBar) sits at the
// top of the client area; this says which part of the window is the title
// bar and which the resize bands, and the system's move and size loops do the
// rest. Dragging an edge keeps the guest screen's shape. Maximized or snapped
// the window has another shape than the guest screen, which the container
// then centers in it.
class WindowsLeanFrame {
public:
    struct Geometry {
        // Device pixels per logical pixel.
        double scale = 1.0;
        // The title bar's height and where its buttons start, in device
        // pixels from the top-left corner of the window's visible part.
        int titleHeight = 0;
        int buttonsLeft = 0;
        // The guest screen's width / height; 0 while it is not known.
        double guestAspect = 0;
        // The smallest guest screen, in device pixels.
        int minGuestWidth = 1;
        int minGuestHeight = 1;
    };

    // How far the window hangs over each edge of its screen's work area, in
    // device pixels.
    struct Overhang {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
    };

    // |sizeMoveDone| is called when the system's move or size loop ends: the
    // window may have been resized, maximized or snapped.
    WindowsLeanFrame(std::function<Geometry()> geometry,
                     std::function<void()> sizeMoveDone);

    // Gives the native window |wid| its frame: the caption, resizable frame
    // and maximize box styles, the frame extended into the client area for
    // the shadow, rounded corners on Windows 11 and the dark border. Safe to
    // repeat, and repeated whenever Qt may have reset the window style.
    static void apply(WId wid);

    // Shows the window's system menu at the screen point (x, y) and carries
    // out the command picked.
    static void showSystemMenu(WId wid, int x, int y);

    // The part of the maximized window |wid| that is off the screen: a
    // maximized window hangs over the work area by its frame, and here the
    // frame is client area. All zero while the window is not maximized.
    static Overhang maximizedOverhang(WId wid);

    // Whether the system holds the window |wid| at a size of its choosing:
    // maximized, or snapped to a part of the screen.
    static bool heldBySystem(WId wid);

    // Handles a message (MSG*) of the window. Returns true, with the window
    // procedure's |result|, when it handled it.
    bool handleMessage(void* message, qintptr* result);

    // Whether the system's move or size loop is running.
    bool inSizeMove() const { return mInSizeMove; }

private:
    std::function<Geometry()> mGeometry;
    std::function<void()> mSizeMoveDone;
    bool mInSizeMove = false;
};
