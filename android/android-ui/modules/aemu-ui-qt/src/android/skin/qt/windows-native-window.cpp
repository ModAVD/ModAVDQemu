/* Copyright (C) 2016 The Android Open Source Project
 **
 ** This software is licensed under the terms of the GNU General Public
 ** License version 2, as published by the Free Software Foundation, and
 ** may be copied, distributed, and modified under those terms.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 */

#include <windows.h>
#include "android/skin/qt/windows-native-window.h"

int numHeldMouseButtons() {

    // We care about the left and right mouse buttons because these functions
    // query the physical mouse state, but those buttons can be re-mapped in
    // the system virtually.
    int numHeld = 0;
    if (GetKeyState(VK_LBUTTON) & 0x80) {
        numHeld++;
    }
    if (GetKeyState(VK_RBUTTON) & 0x80) {
        numHeld++;
    }
    return numHeld;
}

void setDarkTitleBar(WId wid) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm || !wid) {
        return;
    }
    using SetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto setAttr = reinterpret_cast<SetAttr>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (!setAttr) {
        FreeLibrary(dwm);
        return;
    }
    const BOOL dark = TRUE;
    if (FAILED(setAttr(reinterpret_cast<HWND>(wid), 20, &dark, sizeof(dark)))) {
        setAttr(reinterpret_cast<HWND>(wid), 19, &dark, sizeof(dark));
    }
    const DWORD caption = 0x002d2120;
    const DWORD text = 0x00ece8e8;
    setAttr(reinterpret_cast<HWND>(wid), 35, &caption, sizeof(caption));
    setAttr(reinterpret_cast<HWND>(wid), 36, &text, sizeof(text));
    FreeLibrary(dwm);
}
