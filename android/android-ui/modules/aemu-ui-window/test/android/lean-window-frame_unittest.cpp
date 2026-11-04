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

#include <vector>

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

TEST(LeanWindowFrame, DragSameScale) {
    const lw::Point p = lw::dragTopLeft({100, 200}, {150, 210}, 1.0, {450, 610},
                                        1.0);
    EXPECT_EQ(p.x, 400);
    EXPECT_EQ(p.y, 600);
    // Off to the left and above the screen origin.
    const lw::Point q = lw::dragTopLeft({100, 200}, {150, 210}, 1.5, {-20, -5},
                                        1.5);
    EXPECT_EQ(q.x, -70);
    EXPECT_EQ(q.y, -15);
}

TEST(LeanWindowFrame, DragOntoAnotherScale) {
    // Grabbed 300 px right of the window's left edge at 100%; on a 150% screen
    // the same point of the title bar is 450 px in, and stays under the cursor.
    const lw::Point p = lw::dragTopLeft({0, 0}, {300, 16}, 1.0, {3000, 500},
                                        1.5);
    EXPECT_EQ(p.x, 3000 - 450);
    EXPECT_EQ(p.y, 500 - 24);
    // And back.
    const lw::Point q = lw::dragTopLeft({2550, 476}, {3000, 500}, 1.5,
                                        {1000, 300}, 1.0);
    EXPECT_EQ(q.x, 1000 - 300);
    EXPECT_EQ(q.y, 300 - 16);
}

TEST(LeanWindowFrame, DragBadScale) {
    const lw::Point p = lw::dragTopLeft({0, 0}, {30, 10}, 0, {100, 100}, 2.0);
    EXPECT_EQ(p.x, 70);
    EXPECT_EQ(p.y, 90);
}

namespace {

// A 9:16 guest screen 360x640 under a 32 px title bar, at (100, 100).
const lw::Rect kStart = {100, 100, 460, 772};
constexpr double kAspect = 360.0 / 640.0;

lw::Rect resize(Hit hit, int dx, int dy, int minW = 1, int minH = 1) {
    const lw::Point grab = {300, 400};
    return lw::resizeRect(kStart, hit, grab, {grab.x + dx, grab.y + dy}, 32,
                          kAspect, minW, minH);
}

void expectGuest(const lw::Rect& r, int width, int height) {
    EXPECT_EQ(r.width(), width);
    EXPECT_EQ(r.height() - 32, height);
}

}  // namespace

TEST(LeanWindowFrame, ResizeRightKeepsAspectAndTopLeft) {
    const lw::Rect r = resize(Hit::Right, 90, 0);
    expectGuest(r, 450, 800);
    EXPECT_EQ(r.left, 100);
    EXPECT_EQ(r.top, 100);
}

TEST(LeanWindowFrame, ResizeLeftKeepsRightEdge) {
    const lw::Rect r = resize(Hit::Left, -90, 0);
    expectGuest(r, 450, 800);
    EXPECT_EQ(r.right, 460);
    EXPECT_EQ(r.top, 100);
}

TEST(LeanWindowFrame, ResizeBottomGrowsToTheRight) {
    const lw::Rect r = resize(Hit::Bottom, 0, 160);
    expectGuest(r, 450, 800);
    EXPECT_EQ(r.left, 100);
    EXPECT_EQ(r.top, 100);
}

TEST(LeanWindowFrame, ResizeTopKeepsBottomEdge) {
    const lw::Rect r = resize(Hit::Top, 0, -160);
    expectGuest(r, 450, 800);
    EXPECT_EQ(r.bottom, 772);
    EXPECT_EQ(r.left, 100);
}

TEST(LeanWindowFrame, ResizeCornerFollowsTheLongerPull) {
    // Pulled further down than right: the height leads.
    expectGuest(resize(Hit::BottomRight, 10, 160), 450, 800);
    // Pulled further right: the width leads.
    expectGuest(resize(Hit::BottomRight, 90, 10), 450, 800);
    const lw::Rect r = resize(Hit::TopLeft, -90, -10);
    expectGuest(r, 450, 800);
    EXPECT_EQ(r.right, 460);
    EXPECT_EQ(r.bottom, 772);
    const lw::Rect s = resize(Hit::TopRight, 90, 0);
    EXPECT_EQ(s.left, 100);
    EXPECT_EQ(s.bottom, 772);
    const lw::Rect t = resize(Hit::BottomLeft, -90, 0);
    EXPECT_EQ(t.right, 460);
    EXPECT_EQ(t.top, 100);
}

TEST(LeanWindowFrame, ResizeShrinks) {
    expectGuest(resize(Hit::Right, -180, 0), 180, 320);
    expectGuest(resize(Hit::Bottom, 0, -320), 180, 320);
}

TEST(LeanWindowFrame, ResizeStopsAtTheMinimum) {
    // At least 200 wide and 168 high: 200 wide wins for a 9:16 screen.
    const lw::Rect r = resize(Hit::Right, -1000, 0, 200, 168);
    expectGuest(r, 200, 356);
    const lw::Rect s = resize(Hit::Left, 1000, 0, 200, 168);
    expectGuest(s, 200, 356);
    EXPECT_EQ(s.right, 460);
    // A landscape screen is held by its height.
    const lw::Rect wide = lw::resizeRect({0, 0, 640, 392}, Hit::Bottom,
                                         {0, 0}, {0, -1000}, 32, 640.0 / 360.0,
                                         200, 168);
    EXPECT_EQ(wide.height() - 32, 168);
    EXPECT_EQ(wide.width(), 299);
}

TEST(LeanWindowFrame, ResizeIgnoresNonResizeHits) {
    for (Hit hit : {Hit::Caption, Hit::Client, Hit::Nowhere}) {
        const lw::Rect r = resize(hit, 50, 50);
        EXPECT_EQ(r.left, kStart.left);
        EXPECT_EQ(r.top, kStart.top);
        EXPECT_EQ(r.right, kStart.right);
        EXPECT_EQ(r.bottom, kStart.bottom);
    }
    const lw::Rect r = lw::resizeRect(kStart, Hit::Right, {0, 0}, {50, 0}, 32,
                                      0.0, 1, 1);
    EXPECT_EQ(r.right, kStart.right);
}

TEST(LeanWindowFrame, SystemMenuKeepsMinimizeAndClose) {
    EXPECT_TRUE(lw::keepsSystemMenuItem(lw::kScMinimize));
    EXPECT_TRUE(lw::keepsSystemMenuItem(lw::kScClose));
    for (unsigned id : std::vector<unsigned>{lw::kScRestore, lw::kScMove, lw::kScSize,
                        lw::kScMaximize, 0u, 0xF100u /*SC_KEYMENU*/}) {
        EXPECT_FALSE(lw::keepsSystemMenuItem(id)) << std::hex << id;
    }
}

TEST(LeanWindowFrame, SystemMenuItemsToDelete) {
    // Windows' own menu: Restore, Move, Size, Minimize, Maximize, -, Close.
    const std::vector<unsigned> standard = {
            lw::kScRestore, lw::kScMove,    lw::kScSize, lw::kScMinimize,
            lw::kScMaximize, 0,             lw::kScClose};
    EXPECT_EQ(lw::systemMenuItemsToDelete(standard),
              (std::vector<int>{4, 2, 1, 0}));
    // What is left: Minimize, -, Close. Nothing more to delete.
    EXPECT_EQ(lw::systemMenuItemsToDelete(
                      {lw::kScMinimize, 0, lw::kScClose}),
              std::vector<int>{});
}

TEST(LeanWindowFrame, SystemMenuSeparators) {
    // Leading, trailing, doubled separators go.
    EXPECT_EQ(lw::systemMenuItemsToDelete({0, lw::kScMinimize, 0, 0,
                                           lw::kScClose, 0}),
              (std::vector<int>{5, 3, 0}));
    // A separator between a kept and a dropped item goes with it.
    EXPECT_EQ(lw::systemMenuItemsToDelete(
                      {lw::kScMinimize, 0, lw::kScMaximize, 0, lw::kScClose}),
              (std::vector<int>{3, 2}));
    EXPECT_EQ(lw::systemMenuItemsToDelete({lw::kScClose, 0, lw::kScMaximize}),
              (std::vector<int>{2, 1}));
    EXPECT_EQ(lw::systemMenuItemsToDelete({}), std::vector<int>{});
}

TEST(LeanWindowFrame, RefusedSysCommands) {
    EXPECT_TRUE(lw::refusesSysCommand(lw::kScMaximize));
    EXPECT_TRUE(lw::refusesSysCommand(lw::kScMaximize | 2));  // e.g. 0xF032
    EXPECT_TRUE(lw::refusesSysCommand(lw::kScMove));
    EXPECT_TRUE(lw::refusesSysCommand(0xF012));  // SC_DRAGMOVE
    EXPECT_TRUE(lw::refusesSysCommand(lw::kScSize));
    EXPECT_TRUE(lw::refusesSysCommand(lw::kScSize | 8));  // bottom right
    for (unsigned command : std::vector<unsigned>{lw::kScMinimize, lw::kScClose, lw::kScRestore,
                             0xF100u /*SC_KEYMENU*/, 0xF090u /*SC_MOUSEMENU*/,
                             0xF130u /*SC_TASKLIST*/}) {
        EXPECT_FALSE(lw::refusesSysCommand(command)) << std::hex << command;
    }
}
