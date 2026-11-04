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
#include <vector>

namespace lw = android::lean_window;

namespace {

// Resize bands inside the window, in logical pixels: along the left, right
// and bottom edges (over the guest screen), along the top edge (over the
// title bar), and how far the corners reach.
constexpr int kBorder = 5;
constexpr int kTopBorder = 4;
constexpr int kCorner = 16;

HWND hwndOf(void* hwnd) {
    return static_cast<HWND>(hwnd);
}

int devicePixels(double scale, int logical) {
    return std::max(1, static_cast<int>(std::lround(logical * scale)));
}

// The window's DPI scale as Windows has it, which is what it scales the window
// by when it moves onto another screen.
double windowScale(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    const UINT dpi = getDpiForWindow ? getDpiForWindow(hwnd) : 0;
    return dpi ? dpi / 96.0 : 1.0;
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

void pruneSystemMenu(HWND hwnd) {
    HMENU menu = GetSystemMenu(hwnd, FALSE);
    if (!menu) {
        return;
    }
    const int count = GetMenuItemCount(menu);
    std::vector<unsigned> ids;
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW info = {};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_FTYPE | MIIM_ID;
        if (!GetMenuItemInfoW(menu, i, TRUE, &info)) {
            ids.push_back(0xFFFFFFFFu);  // unknown: not kept
            continue;
        }
        ids.push_back((info.fType & MFT_SEPARATOR) ? 0u : info.wID);
    }
    for (int position : lw::systemMenuItemsToDelete(ids)) {
        DeleteMenu(menu, position, MF_BYPOSITION);
    }
}

}  // namespace

WindowsLeanFrame::WindowsLeanFrame(std::function<Geometry()> geometry,
                                   std::function<void()> resizeDone)
    : mGeometry(std::move(geometry)), mResizeDone(std::move(resizeDone)) {}

// static
void WindowsLeanFrame::apply(WId wid) {
    HWND hwnd = reinterpret_cast<HWND>(wid);
    if (!hwnd) {
        return;
    }
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR wanted = (style | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX) &
                            ~static_cast<LONG_PTR>(WS_THICKFRAME | WS_MAXIMIZEBOX);
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
    pruneSystemMenu(hwnd);
}

// static
void WindowsLeanFrame::showSystemMenu(WId wid, int x, int y) {
    HWND hwnd = reinterpret_cast<HWND>(wid);
    if (!hwnd) {
        return;
    }
    pruneSystemMenu(hwnd);
    HMENU menu = GetSystemMenu(hwnd, FALSE);
    if (!menu) {
        return;
    }
    EnableMenuItem(menu, SC_MINIMIZE, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
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

bool WindowsLeanFrame::handleMessage(void* message, qintptr* result) {
    MSG* msg = static_cast<MSG*>(message);
    HWND hwnd = msg->hwnd;
    switch (msg->message) {
        case WM_NCCALCSIZE:
            // The client area is the whole window, for wParam TRUE (the first
            // rectangle stays as proposed) and FALSE alike.
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
            const lw::Layout layout = {window.right - window.left,
                                       window.bottom - window.top,
                                       g.titleHeight,
                                       g.buttonsLeft,
                                       devicePixels(g.scale, kBorder),
                                       devicePixels(g.scale, kTopBorder),
                                       devicePixels(g.scale, kCorner)};
            const lw::Hit hit =
                    lw::hitTest(layout, GET_X_LPARAM(msg->lParam) - window.left,
                                GET_Y_LPARAM(msg->lParam) - window.top);
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

        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK: {
            // A double click on the title bar does not maximize; it is just
            // another press.
            const lw::Hit hit = static_cast<lw::Hit>(msg->wParam);
            if (hit != lw::Hit::Caption && !lw::isResize(hit)) {
                return false;
            }
            startTracking(hwnd, hit, GET_X_LPARAM(msg->lParam),
                          GET_Y_LPARAM(msg->lParam));
            *result = 0;
            return true;
        }

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

        case WM_MOUSEMOVE:
        case WM_LBUTTONUP: {
            if (mMode == Mode::None) {
                return false;
            }
            POINT cursor = {GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
            ClientToScreen(hwnd, &cursor);
            track(hwnd, cursor.x, cursor.y);
            if (msg->message == WM_LBUTTONUP) {
                stopTracking(hwnd, true);
            }
            *result = 0;
            return true;
        }

        case WM_CAPTURECHANGED:
            if (mMode != Mode::None && reinterpret_cast<HWND>(msg->lParam) != hwnd) {
                stopTracking(hwnd, false);
            }
            return false;

        case WM_SYSCOMMAND:
            if (!lw::refusesSysCommand(static_cast<unsigned>(msg->wParam))) {
                return false;
            }
            *result = 0;
            return true;

        default:
            return false;
    }
}

void WindowsLeanFrame::startTracking(void* handle, lw::Hit hit, int x, int y) {
    HWND hwnd = hwndOf(handle);
    RECT window;
    if (!GetWindowRect(hwnd, &window)) {
        return;
    }
    mStartRect = {window.left, window.top, window.right, window.bottom};
    mGrab = {x, y};
    mHit = hit;
    mStartScale = windowScale(hwnd);
    mMode = hit == lw::Hit::Caption ? Mode::Move : Mode::Resize;
    SetCapture(hwnd);
}

void WindowsLeanFrame::track(void* handle, int x, int y) {
    HWND hwnd = hwndOf(handle);
    constexpr UINT kFlags = SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE;
    if (mMode == Mode::Move) {
        const lw::Point topLeft =
                lw::dragTopLeft({mStartRect.left, mStartRect.top}, mGrab,
                                mStartScale, {x, y}, windowScale(hwnd));
        RECT window;
        if (GetWindowRect(hwnd, &window) && window.left == topLeft.x &&
            window.top == topLeft.y) {
            return;
        }
        SetWindowPos(hwnd, nullptr, topLeft.x, topLeft.y, 0, 0,
                     kFlags | SWP_NOSIZE);
    } else if (mMode == Mode::Resize) {
        const Geometry g = mGeometry();
        if (g.guestAspect <= 0) {
            return;
        }
        const lw::Rect r = lw::resizeRect(mStartRect, mHit, mGrab, {x, y},
                                          g.titleHeight, g.guestAspect,
                                          g.minGuestWidth, g.minGuestHeight);
        SetWindowPos(hwnd, nullptr, r.left, r.top, r.width(), r.height(),
                     kFlags);
    }
}

void WindowsLeanFrame::stopTracking(void* handle, bool releaseCapture) {
    const bool resized = mMode == Mode::Resize;
    // Before ReleaseCapture(), which sends WM_CAPTURECHANGED.
    mMode = Mode::None;
    if (releaseCapture && GetCapture() == hwndOf(handle)) {
        ReleaseCapture();
    }
    if (resized && mResizeDone) {
        mResizeDone();
    }
}
