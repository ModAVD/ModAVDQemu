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
#include "android/lean-window-frame.h"

#include <algorithm>
#include <cmath>

namespace android {
namespace lean_window {

Hit hitTest(const Layout& l, int x, int y) {
    if (x < 0 || y < 0 || x >= l.width || y >= l.height) {
        return Hit::Nowhere;
    }
    const bool left = x < l.border;
    const bool right = x >= l.width - l.border;
    const bool top = y < l.topBorder;
    const bool bottom = y >= l.height - l.border;
    const bool nearLeft = x < l.corner;
    const bool nearRight = x >= l.width - l.corner;
    const bool nearTop = y < l.corner;
    const bool nearBottom = y >= l.height - l.corner;

    if (bottom) {
        return nearLeft ? Hit::BottomLeft
                        : (nearRight ? Hit::BottomRight : Hit::Bottom);
    }
    if (top) {
        if (nearLeft) {
            return Hit::TopLeft;
        }
        if (nearRight) {
            return Hit::TopRight;
        }
        // The buttons reach up to the top edge.
        return x >= l.buttonsLeft ? Hit::Client : Hit::Top;
    }
    if (left) {
        return nearTop ? Hit::TopLeft
                       : (nearBottom ? Hit::BottomLeft : Hit::Left);
    }
    if (right) {
        return nearTop ? Hit::TopRight
                       : (nearBottom ? Hit::BottomRight : Hit::Right);
    }
    if (y < l.titleHeight) {
        return x >= l.buttonsLeft ? Hit::Client : Hit::Caption;
    }
    return Hit::Client;
}

bool isResize(Hit hit) {
    return static_cast<int>(hit) >= static_cast<int>(Hit::Left) &&
           static_cast<int>(hit) <= static_cast<int>(Hit::BottomRight);
}

Hit sizingEdge(unsigned wparam) {
    // WMSZ_LEFT (1) ... WMSZ_BOTTOMRIGHT (8) are in the order of HTLEFT (10)
    // ... HTBOTTOMRIGHT (17).
    return wparam >= 1 && wparam <= 8 ? static_cast<Hit>(wparam + 9)
                                      : Hit::Nowhere;
}

Rect sizingRect(const Rect& proposed,
                Hit edge,
                int titleHeight,
                double aspect,
                int minGuestWidth,
                int minGuestHeight) {
    if (!isResize(edge) || aspect <= 0) {
        return proposed;
    }
    const bool left = edge == Hit::Left || edge == Hit::TopLeft ||
                      edge == Hit::BottomLeft;
    const bool right = edge == Hit::Right || edge == Hit::TopRight ||
                       edge == Hit::BottomRight;
    const bool top =
            edge == Hit::Top || edge == Hit::TopLeft || edge == Hit::TopRight;
    const bool bottom = edge == Hit::Bottom || edge == Hit::BottomLeft ||
                        edge == Hit::BottomRight;

    // The guest height each dragged edge asks for; the larger one wins, so a
    // corner follows the cursor along whichever edge it moved further.
    double height = 0;
    if (left || right) {
        height = proposed.width() / aspect;
    }
    if (top || bottom) {
        height = std::max(height,
                          static_cast<double>(proposed.height() - titleHeight));
    }
    height = std::max({height, static_cast<double>(minGuestHeight),
                       minGuestWidth / aspect, 1.0});
    const int guestHeight = static_cast<int>(std::lround(height));
    const int guestWidth = std::max(
            1, static_cast<int>(std::lround(guestHeight * aspect)));

    Rect r = proposed;
    if (left) {
        r.left = proposed.right - guestWidth;
    } else {
        r.right = proposed.left + guestWidth;
    }
    if (top) {
        r.top = proposed.bottom - guestHeight - titleHeight;
    } else {
        r.bottom = proposed.top + titleHeight + guestHeight;
    }
    return r;
}

Rect visiblePart(const Rect& window, const Rect& work) {
    const Rect r = {std::max(window.left, work.left),
                    std::max(window.top, work.top),
                    std::min(window.right, work.right),
                    std::min(window.bottom, work.bottom)};
    return r.width() > 0 && r.height() > 0 ? r : window;
}

}  // namespace lean_window
}  // namespace android
