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

#include <vector>

// The window frame of the lean window (ModAVD#58): the window has no system
// title bar and no resizable frame, the emulator draws the title bar and moves
// and resizes the window itself. This is the arithmetic of it, without any
// windowing system, so that it can be tested on its own. All lengths are in
// device pixels, all points in screen coordinates unless said otherwise.
namespace android {
namespace lean_window {

// Where a point of the window is, with the values of the Win32 HT* codes.
enum class Hit : int {
    Nowhere = 0,
    Client = 1,
    Caption = 2,
    Left = 10,
    Right = 11,
    Top = 12,
    TopLeft = 13,
    TopRight = 14,
    Bottom = 15,
    BottomLeft = 16,
    BottomRight = 17,
};

struct Point {
    int x;
    int y;
};

struct Rect {
    int left;
    int top;
    int right;
    int bottom;
    int width() const { return right - left; }
    int height() const { return bottom - top; }
};

// The parts of the window, relative to its top-left corner.
struct Layout {
    int width;
    int height;
    // The title bar is the band [0, titleHeight) at the top.
    int titleHeight;
    // The title bar buttons take [buttonsLeft, width) of the title bar.
    int buttonsLeft;
    // Resize bands inside the window along the left, right and bottom edges,
    // and along the top edge.
    int border;
    int topBorder;
    // How far from a corner, along either edge, the corner's diagonal resize
    // reaches.
    int corner;
};

// What is under the point (x, y) of the window: a resize band, the part of the
// title bar that moves the window, or the client (the buttons, the guest
// screen). The buttons stay clickable except in the bands of the corners and
// along the right edge.
Hit hitTest(const Layout& layout, int x, int y);

// Whether |hit| resizes the window.
bool isResize(Hit hit);

// Where the window's top-left corner goes while the title bar is dragged: the
// point that was grabbed at |grab|, with the window at |startTopLeft| and the
// screen scale |startScale|, stays under the cursor at |cursor| when the
// window is at scale |scale| (it changes when the window moves onto a screen
// with another scale and Windows resizes it).
Point dragTopLeft(Point startTopLeft,
                  Point grab,
                  double startScale,
                  Point cursor,
                  double scale);

// The window while the resize band |hit| is dragged from |grab| to |cursor|,
// starting from |start|. The guest screen, the window below the title bar of
// |titleHeight|, keeps the ratio |aspect| (width / height) and is at least
// |minGuestWidth| x |minGuestHeight|. The edges opposite the dragged ones stay
// where they are; a side edge grows the window downwards, the top and bottom
// edges grow it to the right.
Rect resizeRect(const Rect& start,
                Hit hit,
                Point grab,
                Point cursor,
                int titleHeight,
                double aspect,
                int minGuestWidth,
                int minGuestHeight);

// Win32 system commands (WM_SYSCOMMAND, low four bits masked off).
enum SysCommand : unsigned {
    kScSize = 0xF000,
    kScMove = 0xF010,
    kScMinimize = 0xF020,
    kScMaximize = 0xF030,
    kScClose = 0xF060,
    kScRestore = 0xF120,
};

// The system menu (Alt+Space, right click on the title bar) keeps only
// minimize and close.
bool keepsSystemMenuItem(unsigned id);

// The positions to delete, last first, from a system menu whose items have
// the ids |ids| (0 for a separator): every item but the kept ones, and every
// separator that would not stand between two kept items.
std::vector<int> systemMenuItemsToDelete(const std::vector<unsigned>& ids);

// Whether the window refuses the system command |wparam| (the WM_SYSCOMMAND
// wParam): maximize, and the keyboard move and size, which would start the
// system's move and size loops and with them Snap.
bool refusesSysCommand(unsigned wparam);

}  // namespace lean_window
}  // namespace android
