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
#include "android/skin/qt/windows-lean-frame.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace lw = android::lean_window;

namespace {

// Resize bands inside the window, in logical pixels: along the left, right
// and bottom edges (over the guest screen), along the top edge (over the
// title bar), and how far the corners reach.
constexpr int kBorder = 5;
constexpr int kTopBorder = 4;
constexpr int kCorner = 16;

int devicePixels(double scale, int logical) {
    return std::max(1, static_cast<int>(std::lround(logical * scale)));
}

// The IDC_SIZE* cursors, by number: this code is built without UNICODE.
const wchar_t* resizeCursor(lw::Hit hit) {
    switch (hit) {
        case lw::Hit::Left:
        case lw::Hit::Right:
            return MAKEINTRESOURCEW(32644);  // IDC_SIZEWE
        case lw::Hit::Top:
        case lw::Hit::Bottom:
            return MAKEINTRESOURCEW(32645);  // IDC_SIZENS
        case lw::Hit::TopLeft:
        case lw::Hit::BottomRight:
            return MAKEINTRESOURCEW(32642);  // IDC_SIZENWSE
        default:
            return MAKEINTRESOURCEW(32643);  // IDC_SIZENESW
    }
}

lw::Rect toRect(const RECT& r) {
    return {r.left, r.top, r.right, r.bottom};
}

// The part of the window that is on the screen: all of it, but for a
// maximized window, which hangs over the work area by its frame.
lw::Rect visibleRect(HWND hwnd, const RECT& window) {
    if (!IsZoomed(hwnd)) {
        return toRect(window);
    }
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                         &info)) {
        return toRect(window);
    }
    return lw::visiblePart(toRect(window), toRect(info.rcWork));
}

}  // namespace

WindowsLeanFrame::WindowsLeanFrame(std::function<Geometry()> geometry,
                                   std::function<void()> sizeMoveDone)
    : mGeometry(std::move(geometry)), mSizeMoveDone(std::move(sizeMoveDone)) {}

// static
void WindowsLeanFrame::apply(WId wid) {
    HWND hwnd = reinterpret_cast<HWND>(wid);
    if (!hwnd) {
        return;
    }
    // The resizable frame and the maximize box are what let the system
    // maximize and snap the window; neither is drawn.
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR wanted = style | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                            WS_THICKFRAME | WS_MAXIMIZEBOX;
    if (wanted != style) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, wanted);
        // Windows asks for the client area again (WM_NCCALCSIZE).
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                             SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    }

    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        struct Margins {
            int left, right, top, bottom;
        };
        using ExtendFrameFn = HRESULT(WINAPI*)(HWND, const Margins*);
        using SetAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        const auto extendFrame = reinterpret_cast<ExtendFrameFn>(
                GetProcAddress(dwm, "DwmExtendFrameIntoClientArea"));
        const auto setAttribute = reinterpret_cast<SetAttributeFn>(
                GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (extendFrame) {
            // Any frame at all in the client area keeps the shadow.
            const Margins margins = {0, 0, 1, 0};
            extendFrame(hwnd, &margins);
        }
        if (setAttribute) {
            // DWMWA_USE_IMMERSIVE_DARK_MODE (19 before Windows 10 20H1): the
            // border around the window.
            const BOOL dark = TRUE;
            if (FAILED(setAttribute(hwnd, 20, &dark, sizeof(dark)))) {
                setAttribute(hwnd, 19, &dark, sizeof(dark));
            }
            // DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND, Windows 11 only.
            const DWORD round = 2;
            setAttribute(hwnd, 33, &round, sizeof(round));
        }
        FreeLibrary(dwm);
    }
}

// static
void WindowsLeanFrame::showSystemMenu(WId wid, int x, int y) {
    HWND hwnd = reinterpret_cast<HWND>(wid);
    if (!hwnd) {
        return;
    }
    HMENU menu = GetSystemMenu(hwnd, FALSE);
    if (!menu) {
        return;
    }
    // What DefWindowProc does for the menu it shows itself.
    const bool maximized = IsZoomed(hwnd);
    const auto enable = [menu](UINT command, bool enabled) {
        EnableMenuItem(menu, command,
                       MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    };
    enable(SC_RESTORE, maximized);
    enable(SC_MOVE, !maximized);
    enable(SC_SIZE, !maximized);
    enable(SC_MINIMIZE, true);
    enable(SC_MAXIMIZE, !maximized);
    enable(SC_CLOSE, true);
    SetMenuDefaultItem(menu, SC_CLOSE, FALSE);
    const UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN
                                                               : TPM_LEFTALIGN;
    const int command = TrackPopupMenuEx(
            menu, align | TPM_TOPALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y,
            hwnd, nullptr);
    if (command) {
        // Close goes the usual way, WM_CLOSE, and so to the safe shutdown.
        PostMessageW(hwnd, WM_SYSCOMMAND, static_cast<WPARAM>(command),
                     MAKELPARAM(x, y));
    }
}

// static
WindowsLeanFrame::Overhang WindowsLeanFrame::maximizedOverhang(WId wid) {
    HWND hwnd = reinterpret_cast<HWND>(wid);
    RECT window;
    if (!hwnd || !IsZoomed(hwnd) || !GetWindowRect(hwnd, &window)) {
        return {};
    }
    const lw::Rect visible = visibleRect(hwnd, window);
    return {visible.left - window.left, visible.top - window.top,
            window.right - visible.right, window.bottom - visible.bottom};
}

// static
bool WindowsLeanFrame::heldBySystem(WId wid) {
    HWND hwnd = reinterpret_cast<HWND>(wid);
    if (!hwnd) {
        return false;
    }
    if (IsZoomed(hwnd)) {
        return true;
    }
    // Windows 10 1903 and later.
    using IsWindowArrangedFn = BOOL(WINAPI*)(HWND);
    static const auto isWindowArranged = reinterpret_cast<IsWindowArrangedFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "IsWindowArranged"));
    return isWindowArranged && isWindowArranged(hwnd);
}

bool WindowsLeanFrame::handleMessage(void* message, qintptr* result) {
    MSG* msg = static_cast<MSG*>(message);
    HWND hwnd = msg->hwnd;
    switch (msg->message) {
        case WM_NCCALCSIZE:
            // The client area is the whole window, for wParam TRUE (the first
            // rectangle stays as proposed) and FALSE alike, maximized too:
            // Qt takes the window's rectangle for the client area of a
            // frameless window, so the two have to stay the same. The part
            // of a maximized window that hangs over the screen is left empty
            // instead, see maximizedOverhang().
            *result = 0;
            return true;

        case WM_NCHITTEST: {
            if (IsIconic(hwnd)) {
                return false;
            }
            RECT window;
            if (!GetWindowRect(hwnd, &window)) {
                return false;
            }
            const Geometry g = mGeometry();
            // A maximized window has no resize bands, and its title bar
            // starts where the screen does.
            const bool maximized = IsZoomed(hwnd);
            const lw::Rect visible = visibleRect(hwnd, window);
            const lw::Layout layout = {
                    visible.width(),
                    visible.height(),
                    g.titleHeight,
                    g.buttonsLeft,
                    maximized ? 0 : devicePixels(g.scale, kBorder),
                    maximized ? 0 : devicePixels(g.scale, kTopBorder),
                    maximized ? 0 : devicePixels(g.scale, kCorner)};
            const lw::Hit hit =
                    lw::hitTest(layout, GET_X_LPARAM(msg->lParam) - visible.left,
                                GET_Y_LPARAM(msg->lParam) - visible.top);
            *result = static_cast<qintptr>(hit);
            return true;
        }

        case WM_SETCURSOR: {
            const lw::Hit hit =
                    static_cast<lw::Hit>(static_cast<short>(LOWORD(msg->lParam)));
            if (reinterpret_cast<HWND>(msg->wParam) != hwnd || !lw::isResize(hit)) {
                return false;
            }
            SetCursor(LoadCursorW(nullptr, resizeCursor(hit)));
            *result = TRUE;
            return true;
        }

        // WM_NCLBUTTONDOWN and WM_NCLBUTTONDBLCLK go on to DefWindowProc: on
        // the title bar it activates the window, moves it in the system's
        // move loop (which snaps it at the edges of the screen and maximizes
        // it at the top) and maximizes or restores it on a double click; on a
        // resize band it starts the system's size loop.

        case WM_NCRBUTTONDOWN:
            if (msg->wParam != HTCAPTION) {
                return false;
            }
            *result = 0;
            return true;

        case WM_NCRBUTTONUP:
            if (msg->wParam != HTCAPTION) {
                return false;
            }
            showSystemMenu(reinterpret_cast<WId>(hwnd), GET_X_LPARAM(msg->lParam),
                           GET_Y_LPARAM(msg->lParam));
            *result = 0;
            return true;

        case WM_SIZING: {
            // An edge or a corner is dragged: the guest screen keeps its
            // shape.
            const lw::Hit edge =
                    lw::sizingEdge(static_cast<unsigned>(msg->wParam));
            const Geometry g = mGeometry();
            RECT* proposed = reinterpret_cast<RECT*>(msg->lParam);
            if (!proposed || !lw::isResize(edge) || g.guestAspect <= 0) {
                return false;
            }
            const lw::Rect r = lw::sizingRect(toRect(*proposed), edge,
                                              g.titleHeight, g.guestAspect,
                                              g.minGuestWidth, g.minGuestHeight);
            proposed->left = r.left;
            proposed->top = r.top;
            proposed->right = r.right;
            proposed->bottom = r.bottom;
            *result = TRUE;
            return true;
        }

        case WM_ENTERSIZEMOVE:
            mInSizeMove = true;
            return false;

        case WM_EXITSIZEMOVE:
            mInSizeMove = false;
            if (mSizeMoveDone) {
                mSizeMoveDone();
            }
            return false;

        default:
            return false;
    }
}
