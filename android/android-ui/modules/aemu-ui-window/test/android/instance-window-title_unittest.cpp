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
#include "android/instance-window-title.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {

std::string title(const char* displayName,
                  const char* avdName,
                  size_t buffLen = 128) {
    std::string buff(buffLen + 1, '#');  // '#' past the end catches overruns
    const size_t len =
            instance_window_title(displayName, avdName, &buff[0], buffLen);
    EXPECT_EQ(buff[buffLen], '#');
    EXPECT_EQ(len, strlen(buff.c_str()));
    return buff.c_str();
}

// "方" is three bytes in UTF-8.
const char kFang[] = "\xE6\x96\xB9";

}  // namespace

TEST(InstanceWindowTitle, DisplayName) {
    EXPECT_EQ(title("\xE6\x96\xB9\xE8\x88\x9F", "mas_1"), "\xE6\x96\xB9\xE8\x88\x9F");
    EXPECT_EQ(title("Daily", "mas_1"), "Daily");
}

TEST(InstanceWindowTitle, NoPortNoPrefix) {
    const std::string t = title("Daily", "mas_1");
    EXPECT_EQ(t.find("Android Emulator"), std::string::npos);
    EXPECT_EQ(t.find(':'), std::string::npos);
}

TEST(InstanceWindowTitle, AvdNameWithoutDisplayName) {
    EXPECT_EQ(title(nullptr, "mas_6"), "mas_6");
    EXPECT_EQ(title("", "mas_6"), "mas_6");
    EXPECT_EQ(title(" \t ", "mas_6"), "mas_6");
}

TEST(InstanceWindowTitle, LeadingBlanksDropped) {
    EXPECT_EQ(title("  \tDaily", "mas_1"), "Daily");
    // Only leading ones; the rest of the name is the user's.
    EXPECT_EQ(title("My  phone ", "mas_1"), "My  phone ");
}

TEST(InstanceWindowTitle, NoNamesAtAll) {
    EXPECT_EQ(title(nullptr, nullptr), "");
    EXPECT_EQ(title("", nullptr), "");
}

TEST(InstanceWindowTitle, CutAt127Bytes) {
    const std::string ascii(200, 'a');
    EXPECT_EQ(title(ascii.c_str(), "mas_1"), std::string(127, 'a'));
    EXPECT_EQ(title(ascii.c_str(), "mas_1", 1000), std::string(127, 'a'));
    const std::string exact(127, 'b');
    EXPECT_EQ(title(exact.c_str(), "mas_1"), exact);
}

TEST(InstanceWindowTitle, CutBetweenUtf8Characters) {
    std::string fifty;
    for (int i = 0; i < 50; ++i) {
        fifty += kFang;  // 150 bytes
    }
    // 127 bytes would end inside the 43rd character: 42 characters remain.
    std::string fortyTwo;
    for (int i = 0; i < 42; ++i) {
        fortyTwo += kFang;
    }
    EXPECT_EQ(title(fifty.c_str(), "mas_1"), fortyTwo);
    // A smaller buffer cuts earlier, still between characters.
    EXPECT_EQ(title(fifty.c_str(), "mas_1", 8), std::string(kFang) + kFang);
    EXPECT_EQ(title(fifty.c_str(), "mas_1", 3), "");
}

TEST(InstanceWindowTitle, BufferEdges) {
    char one[1] = {'x'};
    EXPECT_EQ(instance_window_title("Daily", "mas_1", one, 1), 0u);
    EXPECT_EQ(one[0], '\0');
    char untouched[1] = {'x'};
    EXPECT_EQ(instance_window_title("Daily", "mas_1", untouched, 0), 0u);
    EXPECT_EQ(untouched[0], 'x');
    EXPECT_EQ(instance_window_title("Daily", "mas_1", nullptr, 10), 0u);
}
