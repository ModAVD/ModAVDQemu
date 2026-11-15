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

// The window frame of the lean window (ModAVD#58): the window has no system
// title bar, the emulator draws the title bar and says which part of the
// window is what; the system moves, resizes, maximizes and snaps it
// (ModAVD#161). This is the arithmetic of it, without any windowing system, so
// that it can be tested on its own. All lengths are in device pixels, all
// points in screen coordinates unless said otherwise.
namespace android {
namespace lean_window {

// Where a point of the window is, with the values of the Win32 HT* codes.
enum class Hit : int {
    Nowhere = 0,
    Client = 1,
    Caption = 2,
    // The title bar's maximize / restore button (ModAVD#185): Windows 11
    // shows its snap layouts over it.
    MaxButton = 9,
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
    // The maximize / restore button takes [maxButtonLeft, maxButtonRight) of
    // the title bar, inside the buttons; none while the two are equal.
    int maxButtonLeft = 0;
    int maxButtonRight = 0;
};

// What is under the point (x, y) of the window: a resize band, the part of the
// title bar that moves the window, the maximize / restore button, or the
// client (the other buttons, the guest screen). The buttons stay clickable
// except in the bands of the corners and along the right edge.
Hit hitTest(const Layout& layout, int x, int y);

// The maximize / restore button (ModAVD#185) shows what a click does: maximize
// the window, or restore a maximized one. A snapped window is not maximized.
enum class MaxButtonFace { Maximize, Restore };
MaxButtonFace maxButtonFace(bool maximized);

// The system command a click on the button gives the window, the one a double
// click on the title bar gives too: SC_RESTORE for a maximized window,
// SC_MAXIMIZE for any other.
unsigned maxButtonCommand(bool maximized);

// The button as the window's non-client mouse messages leave it. While the
// window answers WM_NCHITTEST with HTMAXBUTTON for the button, the mouse over
// it is non-client to Windows: the widget gets no mouse events, so its
// highlight and its click come from WM_NCMOUSEMOVE, WM_NCLBUTTONDOWN,
// WM_NCLBUTTONUP and WM_NCMOUSELEAVE here, each with the hit-test code of
// the point. Without the mouse captured, a press that leaves the button is
// given up: a release elsewhere never comes here.
struct MaxButtonTracker {
    // The mouse is over the button.
    bool hot = false;
    // The left button went down on it and has not been released.
    bool down = false;

    // WM_NCMOUSEMOVE.
    void move(Hit hit);
    // WM_NCLBUTTONDOWN or WM_NCLBUTTONDBLCLK.
    void press(Hit hit);
    // WM_NCLBUTTONUP; true when that completes a click on the button.
    bool release(Hit hit);
    // WM_NCMOUSELEAVE: the mouse left the non-client part of the window.
    void leave();
};

// Whether |hit| resizes the window.
bool isResize(Hit hit);

// The edge or corner the system's size loop drags, from the wParam of
// WM_SIZING (WMSZ_LEFT ... WMSZ_BOTTOMRIGHT); Hit::Nowhere for anything else.
Hit sizingEdge(unsigned wparam);

// The window the system's size loop gets instead of |proposed| while |edge| is
// dragged (WM_SIZING). The guest screen, the window below the title bar of
// |titleHeight|, keeps the ratio |aspect| (width / height) and is at least
// |minGuestWidth| x |minGuestHeight|. The edges opposite the dragged ones stay
// where they are; a side edge grows the window downwards, the top and bottom
// edges grow it to the right, a corner follows the edge that asks for more.
Rect sizingRect(const Rect& proposed,
                Hit edge,
                int titleHeight,
                double aspect,
                int minGuestWidth,
                int minGuestHeight);

// The part of the maximized window |window| that is on the screen: Windows
// lets a maximized window with a resizable frame hang over the work area
// |work| of its screen by the width of that frame on every side.
Rect visiblePart(const Rect& window, const Rect& work);

}  // namespace lean_window
}  // namespace android
