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
#include "android/lean-title-bar-logic.h"

#include <gtest/gtest.h>

namespace tb = android::lean_title_bar;
using tb::Item;

TEST(LeanTitleBarLogic, ItemsWithVolume) {
    const std::vector<Item> expected = {
            Item::Screenshot, Item::Volume,   Item::Back,     Item::Home,
            Item::Divider,    Item::Minimize, Item::Maximize, Item::Close};
    EXPECT_EQ(tb::items(true), expected);
}

TEST(LeanTitleBarLogic, ItemsWithoutVolume) {
    const std::vector<Item> expected = {Item::Screenshot, Item::Back,
                                        Item::Home,       Item::Divider,
                                        Item::Minimize,   Item::Maximize,
                                        Item::Close};
    EXPECT_EQ(tb::items(false), expected);
}

TEST(LeanTitleBarLogic, VolumeButtonShown) {
    // The manager leaves -audio out when the instance's sound is on.
    EXPECT_TRUE(tb::volumeButtonShown(nullptr, false));
    EXPECT_TRUE(tb::volumeButtonShown("", false));
    EXPECT_TRUE(tb::volumeButtonShown("dsound", false));
    // ... and passes -audio none when it is off.
    EXPECT_FALSE(tb::volumeButtonShown("none", false));
    EXPECT_FALSE(tb::volumeButtonShown(nullptr, true));
    EXPECT_FALSE(tb::volumeButtonShown("dsound", true));
    EXPECT_TRUE(tb::volumeButtonShown("nonex", false));
}

TEST(LeanTitleBarLogic, AdbServerPortUsable) {
    EXPECT_TRUE(tb::adbServerPortUsable("20089"));
    EXPECT_TRUE(tb::adbServerPortUsable("1"));
    EXPECT_TRUE(tb::adbServerPortUsable("65535"));
    EXPECT_TRUE(tb::adbServerPortUsable("5038"));
    EXPECT_FALSE(tb::adbServerPortUsable(nullptr));
    EXPECT_FALSE(tb::adbServerPortUsable(""));
    EXPECT_FALSE(tb::adbServerPortUsable("5037"));
    EXPECT_FALSE(tb::adbServerPortUsable("05037"));
    EXPECT_FALSE(tb::adbServerPortUsable("0"));
    EXPECT_FALSE(tb::adbServerPortUsable("65536"));
    EXPECT_FALSE(tb::adbServerPortUsable("123456789012"));
    EXPECT_FALSE(tb::adbServerPortUsable("-1"));
    EXPECT_FALSE(tb::adbServerPortUsable("0x4E89"));
    EXPECT_FALSE(tb::adbServerPortUsable(" 20089"));
    EXPECT_FALSE(tb::adbServerPortUsable("20089 "));
}

TEST(LeanTitleBarLogic, ShowVolumePanelArgs) {
    // su first (the shell's own cmd media_session is refused on API 34), the
    // shell after; --adj same shows the panel without changing the volume.
    EXPECT_EQ(tb::showVolumePanelArgs(),
              (std::vector<std::string>{
                      "shell", "su", "0", "cmd", "media_session", "volume",
                      "--show", "--stream", "3", "--adj", "same", "||", "cmd",
                      "media_session", "volume", "--show", "--stream", "3",
                      "--adj", "same"}));
}
