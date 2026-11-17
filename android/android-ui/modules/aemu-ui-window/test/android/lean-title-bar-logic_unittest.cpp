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

TEST(LeanTitleBarLogic, PopupLeft) {
    // Aligned with the button when it fits.
    EXPECT_EQ(tb::popupLeft(100, 200, 0, 1920), 100);
    EXPECT_EQ(tb::popupLeft(1720, 200, 0, 1920), 1720);
    // Ends at the screen's right edge when it would not.
    EXPECT_EQ(tb::popupLeft(1721, 200, 0, 1920), 1720);
    EXPECT_EQ(tb::popupLeft(1900, 200, 0, 1920), 1720);
    // But never left of the screen.
    EXPECT_EQ(tb::popupLeft(-50, 200, 0, 1920), 0);
    EXPECT_EQ(tb::popupLeft(50, 300, 0, 200), 0);
    // A screen left of the primary one.
    EXPECT_EQ(tb::popupLeft(-300, 200, -1920, 0), -300);
    EXPECT_EQ(tb::popupLeft(-100, 200, -1920, 0), -200);
}

TEST(LeanTitleBarLogic, VolumeArgs) {
    EXPECT_EQ(tb::getMusicVolumeArgs(),
              (std::vector<std::string>{"shell", "cmd", "media_session",
                                        "volume", "--stream", "3", "--get"}));
    // su first (the shell's own set is refused on API 34), the shell after.
    EXPECT_EQ(tb::setMusicVolumeArgs(7),
              (std::vector<std::string>{
                      "shell", "su", "0", "cmd", "media_session", "volume",
                      "--stream", "3", "--set", "7", "||", "cmd",
                      "media_session", "volume", "--stream", "3", "--set",
                      "7"}));
    EXPECT_EQ(tb::setMusicVolumeArgs(0).back(), "0");
    EXPECT_EQ(tb::setMusicVolumeArgs(0)[9], "0");
}

TEST(LeanTitleBarLogic, ParseStreamVolume) {
    const auto volume = tb::parseStreamVolume(
            "[V] will get volume\r\n"
            "[V] Connected to AudioService\r\n"
            "[V] volume is 5 in range [0..15]\r\n");
    ASSERT_TRUE(volume);
    EXPECT_EQ(volume->index, 5);
    EXPECT_EQ(volume->min, 0);
    EXPECT_EQ(volume->max, 15);
    EXPECT_FALSE(volume->muted());

    const auto bare = tb::parseStreamVolume("volume is 0 in range [0..25]");
    ASSERT_TRUE(bare);
    EXPECT_EQ(bare->index, 0);
    EXPECT_EQ(bare->max, 25);
    EXPECT_TRUE(bare->muted());

    const auto top = tb::parseStreamVolume("volume is 15 in range [0..15]\n");
    ASSERT_TRUE(top);
    EXPECT_EQ(top->index, 15);

    const auto lowMin = tb::parseStreamVolume("volume is 1 in range [1..7]");
    ASSERT_TRUE(lowMin);
    EXPECT_TRUE(lowMin->muted());
    EXPECT_EQ(lowMin->min, 1);
}

TEST(LeanTitleBarLogic, ParseStreamVolumeSkipsWhatDoesNotAddUp) {
    EXPECT_FALSE(tb::parseStreamVolume(""));
    EXPECT_FALSE(tb::parseStreamVolume("Error: no valid stream"));
    EXPECT_FALSE(tb::parseStreamVolume("volume is  in range [0..15]"));
    EXPECT_FALSE(tb::parseStreamVolume("volume is 5 in range [0..15"));
    EXPECT_FALSE(tb::parseStreamVolume("volume is 5 in range [0.15]"));
    EXPECT_FALSE(tb::parseStreamVolume("volume is 5 of range [0..15]"));
    EXPECT_FALSE(tb::parseStreamVolume("volume is 16 in range [0..15]"));
    EXPECT_FALSE(tb::parseStreamVolume("volume is -1 in range [0..15]"));
    EXPECT_FALSE(tb::parseStreamVolume("volume is 3 in range [5..5]"));
    // A range with nothing to slide over.
    EXPECT_FALSE(tb::parseStreamVolume("volume is 5 in range [5..5]"));
    EXPECT_FALSE(tb::parseStreamVolume("volume is 3 in range [9..5]"));
    EXPECT_FALSE(
            tb::parseStreamVolume("volume is 99999999999 in range [0..15]"));
    // A later line that adds up wins over an earlier one that does not.
    const auto later = tb::parseStreamVolume(
            "volume is x\nvolume is 4 in range [0..15]\n");
    ASSERT_TRUE(later);
    EXPECT_EQ(later->index, 4);
}

TEST(LeanTitleBarLogic, UnmuteIndex) {
    EXPECT_EQ(tb::unmuteIndex(9, 0, 15), 9);
    EXPECT_EQ(tb::unmuteIndex(15, 0, 15), 15);
    EXPECT_EQ(tb::unmuteIndex(1, 0, 15), 1);
    // Nothing remembered: half way up.
    EXPECT_EQ(tb::unmuteIndex(0, 0, 15), 8);
    EXPECT_EQ(tb::unmuteIndex(-1, 0, 15), 8);
    // Remembered from a range the guest no longer has.
    EXPECT_EQ(tb::unmuteIndex(20, 0, 15), 8);
    EXPECT_EQ(tb::unmuteIndex(0, 0, 1), 1);
    EXPECT_EQ(tb::unmuteIndex(2, 2, 3), 3);
}

TEST(LeanTitleBarLogic, VolumeSenderSendsAtOnceWhenIdle) {
    tb::VolumeSender sender;
    EXPECT_FALSE(sender.busy());
    EXPECT_EQ(sender.request(5), 5);
    EXPECT_TRUE(sender.busy());
    EXPECT_EQ(sender.finished(), std::nullopt);
    EXPECT_FALSE(sender.busy());
    EXPECT_EQ(sender.request(6), 6);
}

TEST(LeanTitleBarLogic, VolumeSenderKeepsOnlyTheNewest) {
    tb::VolumeSender sender;
    EXPECT_EQ(sender.request(5), 5);
    EXPECT_EQ(sender.request(6), std::nullopt);
    EXPECT_EQ(sender.request(7), std::nullopt);
    EXPECT_EQ(sender.request(8), std::nullopt);
    EXPECT_EQ(sender.finished(), 8);
    EXPECT_TRUE(sender.busy());
    EXPECT_EQ(sender.finished(), std::nullopt);
    EXPECT_FALSE(sender.busy());
}

TEST(LeanTitleBarLogic, VolumeSenderSkipsWhatWasJustSent) {
    tb::VolumeSender sender;
    EXPECT_EQ(sender.request(5), 5);
    EXPECT_EQ(sender.request(6), std::nullopt);
    EXPECT_EQ(sender.request(5), std::nullopt);
    // Dragged away and back: the guest already has 5.
    EXPECT_EQ(sender.finished(), std::nullopt);
    EXPECT_FALSE(sender.busy());
    EXPECT_EQ(sender.request(5), 5);
}
