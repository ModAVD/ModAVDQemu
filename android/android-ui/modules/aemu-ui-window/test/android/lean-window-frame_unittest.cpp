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

#include <gtest/gtest.h>

namespace lw = android::lean_window;
using lw::Hit;

namespace {

// A 720x1312 guest screen under a 32 px title bar at 100%; the buttons take
// the last 4 x 40 + 9 px.
const lw::Layout kLayout = {/*width=*/720, /*height=*/1312,
                            /*titleHeight=*/32, /*buttonsLeft=*/551,
                            /*border=*/5, /*topBorder=*/4, /*corner=*/16};

Hit at(int x, int y) {
    return lw::hitTest(kLayout, x, y);
}

}  // namespace

TEST(LeanWindowFrame, HitTestTitleBar) {
    EXPECT_EQ(at(100, 16), Hit::Caption);
    EXPECT_EQ(at(5, 4), Hit::Caption);          // just inside both bands
    EXPECT_EQ(at(550, 31), Hit::Caption);       // last pixel before buttons
    EXPECT_EQ(at(551, 16), Hit::Client);        // first button
    EXPECT_EQ(at(714, 16), Hit::Client);        // close, left of the border
    EXPECT_EQ(at(100, 32), Hit::Client);        // guest screen below
}

TEST(LeanWindowFrame, HitTestGuestScreen) {
    EXPECT_EQ(at(360, 600), Hit::Client);
    EXPECT_EQ(at(5, 600), Hit::Client);
    EXPECT_EQ(at(714, 600), Hit::Client);
    EXPECT_EQ(at(360, 1306), Hit::Client);
}

TEST(LeanWindowFrame, HitTestEdges) {
    EXPECT_EQ(at(0, 600), Hit::Left);
    EXPECT_EQ(at(4, 600), Hit::Left);
    EXPECT_EQ(at(719, 600), Hit::Right);
    EXPECT_EQ(at(715, 600), Hit::Right);
    EXPECT_EQ(at(360, 1311), Hit::Bottom);
    EXPECT_EQ(at(360, 1307), Hit::Bottom);
    EXPECT_EQ(at(100, 0), Hit::Top);
    EXPECT_EQ(at(100, 3), Hit::Top);
    // Along the top edge the buttons stay buttons, up to the very top.
    EXPECT_EQ(at(600, 0), Hit::Client);
    // The right edge runs past the close button.
    EXPECT_EQ(at(717, 20), Hit::Right);
}

TEST(LeanWindowFrame, HitTestCorners) {
    EXPECT_EQ(at(0, 0), Hit::TopLeft);
    EXPECT_EQ(at(15, 2), Hit::TopLeft);    // along the top edge
    EXPECT_EQ(at(2, 15), Hit::TopLeft);    // along the left edge
    EXPECT_EQ(at(16, 2), Hit::Top);
    EXPECT_EQ(at(2, 16), Hit::Left);
    EXPECT_EQ(at(719, 0), Hit::TopRight);  // over the close button
    EXPECT_EQ(at(704, 3), Hit::TopRight);
    EXPECT_EQ(at(703, 3), Hit::Client);    // close button, top edge
    EXPECT_EQ(at(718, 15), Hit::TopRight);
    EXPECT_EQ(at(0, 1311), Hit::BottomLeft);
    EXPECT_EQ(at(15, 1308), Hit::BottomLeft);
    EXPECT_EQ(at(2, 1296), Hit::BottomLeft);
    EXPECT_EQ(at(2, 1295), Hit::Left);
    EXPECT_EQ(at(719, 1311), Hit::BottomRight);
    EXPECT_EQ(at(704, 1309), Hit::BottomRight);
    EXPECT_EQ(at(703, 1309), Hit::Bottom);
}

TEST(LeanWindowFrame, HitTestOutside) {
    EXPECT_EQ(at(-1, 10), Hit::Nowhere);
    EXPECT_EQ(at(10, -1), Hit::Nowhere);
    EXPECT_EQ(at(720, 10), Hit::Nowhere);
    EXPECT_EQ(at(10, 1312), Hit::Nowhere);
}

TEST(LeanWindowFrame, HitTestScaled) {
    // At 150% everything is half as big again.
    const lw::Layout l = {1080, 1968, 48, 827, 8, 6, 24};
    EXPECT_EQ(lw::hitTest(l, 300, 40), Hit::Caption);
    EXPECT_EQ(lw::hitTest(l, 300, 48), Hit::Client);
    EXPECT_EQ(lw::hitTest(l, 826, 20), Hit::Caption);
    EXPECT_EQ(lw::hitTest(l, 827, 20), Hit::Client);
    EXPECT_EQ(lw::hitTest(l, 7, 900), Hit::Left);
    EXPECT_EQ(lw::hitTest(l, 8, 900), Hit::Client);
    EXPECT_EQ(lw::hitTest(l, 300, 5), Hit::Top);
    EXPECT_EQ(lw::hitTest(l, 300, 6), Hit::Caption);
}

TEST(LeanWindowFrame, IsResize) {
    for (Hit hit : {Hit::Left, Hit::Right, Hit::Top, Hit::TopLeft,
                    Hit::TopRight, Hit::Bottom, Hit::BottomLeft,
                    Hit::BottomRight}) {
        EXPECT_TRUE(lw::isResize(hit)) << static_cast<int>(hit);
    }
    for (Hit hit : {Hit::Nowhere, Hit::Client, Hit::Caption}) {
        EXPECT_FALSE(lw::isResize(hit)) << static_cast<int>(hit);
    }
}

TEST(LeanWindowFrame, HitTestMaximized) {
    // Maximized there are no resize bands: the title bar reaches the top edge
    // and the corners.
    const lw::Layout l = {1920, 1032, 32, 1751, 0, 0, 0};
    EXPECT_EQ(lw::hitTest(l, 0, 0), Hit::Caption);
    EXPECT_EQ(lw::hitTest(l, 900, 0), Hit::Caption);
    EXPECT_EQ(lw::hitTest(l, 1919, 0), Hit::Client);  // close button
    EXPECT_EQ(lw::hitTest(l, 0, 500), Hit::Client);
    EXPECT_EQ(lw::hitTest(l, 1919, 1031), Hit::Client);
    EXPECT_EQ(lw::hitTest(l, 1920, 500), Hit::Nowhere);
}

TEST(LeanWindowFrame, SizingEdge) {
    EXPECT_EQ(lw::sizingEdge(1), Hit::Left);         // WMSZ_LEFT
    EXPECT_EQ(lw::sizingEdge(2), Hit::Right);
    EXPECT_EQ(lw::sizingEdge(3), Hit::Top);
    EXPECT_EQ(lw::sizingEdge(4), Hit::TopLeft);
    EXPECT_EQ(lw::sizingEdge(5), Hit::TopRight);
    EXPECT_EQ(lw::sizingEdge(6), Hit::Bottom);
    EXPECT_EQ(lw::sizingEdge(7), Hit::BottomLeft);
    EXPECT_EQ(lw::sizingEdge(8), Hit::BottomRight);  // WMSZ_BOTTOMRIGHT
    EXPECT_EQ(lw::sizingEdge(0), Hit::Nowhere);
    EXPECT_EQ(lw::sizingEdge(9), Hit::Nowhere);
}

namespace {

// A 9:16 guest screen 360x640 under a 32 px title bar, at (100, 100).
const lw::Rect kStart = {100, 100, 460, 772};
constexpr double kAspect = 360.0 / 640.0;

// What the size loop proposes after |edge| was dragged by (dx, dy): only the
// dragged edges have moved.
lw::Rect sizing(Hit edge, int dx, int dy, int minW = 1, int minH = 1) {
    lw::Rect proposed = kStart;
    if (edge == Hit::Left || edge == Hit::TopLeft || edge == Hit::BottomLeft) {
        proposed.left += dx;
    }
    if (edge == Hit::Right || edge == Hit::TopRight ||
        edge == Hit::BottomRight) {
        proposed.right += dx;
    }
    if (edge == Hit::Top || edge == Hit::TopLeft || edge == Hit::TopRight) {
        proposed.top += dy;
    }
    if (edge == Hit::Bottom || edge == Hit::BottomLeft ||
        edge == Hit::BottomRight) {
        proposed.bottom += dy;
    }
    return lw::sizingRect(proposed, edge, 32, kAspect, minW, minH);
}

void expectGuest(const lw::Rect& r, int width, int height) {
    EXPECT_EQ(r.width(), width);
    EXPECT_EQ(r.height() - 32, height);
}

}  // namespace

TEST(LeanWindowFrame, SizingRightKeepsAspectAndTopLeft) {
    const lw::Rect r = sizing(Hit::Right, 90, 0);
    expectGuest(r, 450, 800);
    EXPECT_EQ(r.left, 100);
    EXPECT_EQ(r.top, 100);
}

TEST(LeanWindowFrame, SizingLeftKeepsRightEdge) {
    const lw::Rect r = sizing(Hit::Left, -90, 0);
    expectGuest(r, 450, 800);
    EXPECT_EQ(r.right, 460);
    EXPECT_EQ(r.top, 100);
}

TEST(LeanWindowFrame, SizingBottomGrowsToTheRight) {
    const lw::Rect r = sizing(Hit::Bottom, 0, 160);
    expectGuest(r, 450, 800);
    EXPECT_EQ(r.left, 100);
    EXPECT_EQ(r.top, 100);
}

TEST(LeanWindowFrame, SizingTopKeepsBottomEdge) {
    const lw::Rect r = sizing(Hit::Top, 0, -160);
    expectGuest(r, 450, 800);
    EXPECT_EQ(r.bottom, 772);
    EXPECT_EQ(r.left, 100);
}

TEST(LeanWindowFrame, SizingCornerFollowsTheLongerPull) {
    // Pulled further down than right: the height leads.
    expectGuest(sizing(Hit::BottomRight, 10, 160), 450, 800);
    // Pulled further right: the width leads.
    expectGuest(sizing(Hit::BottomRight, 90, 10), 450, 800);
    const lw::Rect r = sizing(Hit::TopLeft, -90, -10);
    expectGuest(r, 450, 800);
    EXPECT_EQ(r.right, 460);
    EXPECT_EQ(r.bottom, 772);
    const lw::Rect s = sizing(Hit::TopRight, 90, 0);
    EXPECT_EQ(s.left, 100);
    EXPECT_EQ(s.bottom, 772);
    const lw::Rect t = sizing(Hit::BottomLeft, -90, 0);
    EXPECT_EQ(t.right, 460);
    EXPECT_EQ(t.top, 100);
}

TEST(LeanWindowFrame, SizingShrinks) {
    expectGuest(sizing(Hit::Right, -180, 0), 180, 320);
    expectGuest(sizing(Hit::Bottom, 0, -320), 180, 320);
}

TEST(LeanWindowFrame, SizingFromAnotherShape) {
    // A snapped window, half a 1920x1032 work area, with the guest screen
    // letterboxed in it: dragging an edge brings it back to the guest's shape.
    const lw::Rect snapped = {0, 0, 960, 1032};
    lw::Rect proposed = snapped;
    proposed.right -= 60;  // the right edge, 60 px to the left
    const lw::Rect r =
            lw::sizingRect(proposed, Hit::Right, 32, 1280.0 / 720.0, 1, 1);
    EXPECT_EQ(r.left, 0);
    EXPECT_EQ(r.top, 0);
    EXPECT_EQ(r.width(), 900);
    EXPECT_EQ(r.height() - 32, 506);
}

TEST(LeanWindowFrame, SizingStopsAtTheMinimum) {
    // At least 200 wide and 168 high: 200 wide wins for a 9:16 screen.
    const lw::Rect r = sizing(Hit::Right, -1000, 0, 200, 168);
    expectGuest(r, 200, 356);
    const lw::Rect s = sizing(Hit::Left, 1000, 0, 200, 168);
    expectGuest(s, 200, 356);
    EXPECT_EQ(s.right, 460);
    // A landscape screen is held by its height.
    const lw::Rect wide = lw::sizingRect({0, 0, 640, 40}, Hit::Bottom, 32,
                                         640.0 / 360.0, 200, 168);
    EXPECT_EQ(wide.height() - 32, 168);
    EXPECT_EQ(wide.width(), 299);
}

TEST(LeanWindowFrame, SizingIgnoresNonResizeEdges) {
    for (Hit edge : {Hit::Caption, Hit::Client, Hit::Nowhere}) {
        const lw::Rect r = sizing(edge, 50, 50);
        EXPECT_EQ(r.left, kStart.left);
        EXPECT_EQ(r.top, kStart.top);
        EXPECT_EQ(r.right, kStart.right);
        EXPECT_EQ(r.bottom, kStart.bottom);
    }
    const lw::Rect proposed = {100, 100, 510, 772};
    const lw::Rect r = lw::sizingRect(proposed, Hit::Right, 32, 0.0, 1, 1);
    EXPECT_EQ(r.right, proposed.right);
    EXPECT_EQ(r.bottom, proposed.bottom);
}

TEST(LeanWindowFrame, VisiblePartOfAMaximizedWindow) {
    // A 1920x1080 screen with a 48 px task bar at the bottom; the maximized
    // window hangs over the work area by its 8 px frame.
    const lw::Rect work = {0, 0, 1920, 1032};
    const lw::Rect r = lw::visiblePart({-8, -8, 1928, 1040}, work);
    EXPECT_EQ(r.left, 0);
    EXPECT_EQ(r.top, 0);
    EXPECT_EQ(r.right, 1920);
    EXPECT_EQ(r.bottom, 1032);
    // On a screen to the left, at 150%: a 12 px frame.
    const lw::Rect s = lw::visiblePart({-2572, -12, 12, 1452},
                                       {-2560, 0, 0, 1440});
    EXPECT_EQ(s.left, -2560);
    EXPECT_EQ(s.top, 0);
    EXPECT_EQ(s.right, 0);
    EXPECT_EQ(s.bottom, 1440);
    // A window inside the work area is all there.
    const lw::Rect t = lw::visiblePart({100, 100, 500, 400}, work);
    EXPECT_EQ(t.left, 100);
    EXPECT_EQ(t.bottom, 400);
    // A window that is not on that screen at all stays as it is.
    const lw::Rect u = lw::visiblePart({3000, 100, 3400, 400}, work);
    EXPECT_EQ(u.left, 3000);
    EXPECT_EQ(u.right, 3400);
}
