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

Point dragTopLeft(Point startTopLeft,
                  Point grab,
                  double startScale,
                  Point cursor,
                  double scale) {
    const double ratio = startScale > 0 && scale > 0 ? scale / startScale : 1.0;
    const int offsetX = static_cast<int>(
            std::lround((grab.x - startTopLeft.x) * ratio));
    const int offsetY = static_cast<int>(
            std::lround((grab.y - startTopLeft.y) * ratio));
    return {cursor.x - offsetX, cursor.y - offsetY};
}

Rect resizeRect(const Rect& start,
                Hit hit,
                Point grab,
                Point cursor,
                int titleHeight,
                double aspect,
                int minGuestWidth,
                int minGuestHeight) {
    if (!isResize(hit) || aspect <= 0) {
        return start;
    }
    const bool left = hit == Hit::Left || hit == Hit::TopLeft ||
                      hit == Hit::BottomLeft;
    const bool right = hit == Hit::Right || hit == Hit::TopRight ||
                       hit == Hit::BottomRight;
    const bool top =
            hit == Hit::Top || hit == Hit::TopLeft || hit == Hit::TopRight;
    const bool bottom = hit == Hit::Bottom || hit == Hit::BottomLeft ||
                        hit == Hit::BottomRight;
    const int dx = cursor.x - grab.x;
    const int dy = cursor.y - grab.y;
    const double startWidth = start.width();
    const double startHeight = std::max(1, start.height() - titleHeight);

    // The guest height each dragged edge asks for; the larger one wins, so a
    // corner follows the cursor along whichever edge it moved further.
    double height = 0;
    if (left || right) {
        const double width = startWidth + (right ? dx : -dx);
        height = width / aspect;
    }
    if (top || bottom) {
        height = std::max(height, startHeight + (bottom ? dy : -dy));
    }
    height = std::max({height, static_cast<double>(minGuestHeight),
                       minGuestWidth / aspect, 1.0});
    const int guestHeight = static_cast<int>(std::lround(height));
    const int guestWidth = std::max(
            1, static_cast<int>(std::lround(guestHeight * aspect)));

    Rect r = start;
    if (left) {
        r.left = start.right - guestWidth;
    } else {
        r.right = start.left + guestWidth;
    }
    if (top) {
        r.top = start.bottom - guestHeight - titleHeight;
    } else {
        r.bottom = start.top + titleHeight + guestHeight;
    }
    return r;
}

bool keepsSystemMenuItem(unsigned id) {
    return id == kScMinimize || id == kScClose;
}

std::vector<int> systemMenuItemsToDelete(const std::vector<unsigned>& ids) {
    std::vector<int> remove;
    bool keptBefore = false;  // a kept item before the current position
    int pendingSeparator = -1;
    for (int i = 0; i < static_cast<int>(ids.size()); ++i) {
        if (ids[i] == 0) {
            if (keptBefore && pendingSeparator < 0) {
                pendingSeparator = i;  // kept if another kept item follows
            } else {
                remove.push_back(i);
            }
        } else if (keepsSystemMenuItem(ids[i])) {
            keptBefore = true;
            pendingSeparator = -1;
        } else {
            remove.push_back(i);
        }
    }
    if (pendingSeparator >= 0) {
        remove.push_back(pendingSeparator);
    }
    std::sort(remove.rbegin(), remove.rend());
    return remove;
}

bool refusesSysCommand(unsigned wparam) {
    const unsigned command = wparam & 0xFFF0;
    return command == kScMaximize || command == kScMove || command == kScSize;
}

}  // namespace lean_window
}  // namespace android
