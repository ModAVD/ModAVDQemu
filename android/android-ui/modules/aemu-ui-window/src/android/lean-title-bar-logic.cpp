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

#include <algorithm>
#include <cstring>

namespace android {
namespace lean_title_bar {

namespace {

// adb's own default server port.
constexpr long kDefaultAdbServerPort = 5037;
// AudioSystem.STREAM_MUSIC.
constexpr char kMusicStream[] = "3";

// Reads a decimal number, maybe negative, at |*pos| of |text| and moves past
// it.
bool readInt(std::string_view text, size_t* pos, int* value) {
    size_t i = *pos;
    bool negative = false;
    if (i < text.size() && text[i] == '-') {
        negative = true;
        ++i;
    }
    const size_t digits = i;
    long result = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        result = result * 10 + (text[i] - '0');
        if (result > 1000000) {
            return false;
        }
        ++i;
    }
    if (i == digits) {
        return false;
    }
    *value = static_cast<int>(negative ? -result : result);
    *pos = i;
    return true;
}

bool readText(std::string_view text, size_t* pos, std::string_view expected) {
    if (text.substr(*pos, expected.size()) != expected) {
        return false;
    }
    *pos += expected.size();
    return true;
}

}  // namespace

std::vector<Item> items(bool volumeShown) {
    std::vector<Item> result = {Item::Screenshot};
    if (volumeShown) {
        result.push_back(Item::Volume);
    }
    result.insert(result.end(), {Item::Back, Item::Home, Item::Divider,
                                 Item::Minimize, Item::Maximize, Item::Close});
    return result;
}

bool volumeButtonShown(const char* audioBackend, bool noAudio) {
    if (noAudio) {
        return false;
    }
    return !audioBackend || std::strcmp(audioBackend, "none") != 0;
}

bool adbServerPortUsable(const char* value) {
    if (!value || !*value) {
        return false;
    }
    long port = 0;
    for (const char* c = value; *c; ++c) {
        if (*c < '0' || *c > '9') {
            return false;
        }
        port = port * 10 + (*c - '0');
        if (port > 65535) {
            return false;
        }
    }
    return port >= 1 && port != kDefaultAdbServerPort;
}

int popupLeft(int anchorLeft, int popupWidth, int screenLeft, int screenRight) {
    int left = anchorLeft;
    if (left + popupWidth > screenRight) {
        left = screenRight - popupWidth;
    }
    return std::max(left, screenLeft);
}

std::vector<std::string> getMusicVolumeArgs() {
    return {"shell", "cmd", "media_session", "volume", "--stream",
            kMusicStream, "--get"};
}

// On API 34 the shell's own `cmd media_session volume --set` is turned down
// (AppOps: com.android.server.media from uid 2000 may not
// AUDIO_MEDIA_VOLUME), while reading works. So the set goes through su first
// and falls back to the shell only where there is no su. adb shell joins the
// words and runs them in the guest's sh, so the || is the guest's.
std::vector<std::string> setMusicVolumeArgs(int index) {
    const std::string value = std::to_string(index);
    return {"shell",  "su",           "0",      "cmd",      "media_session",
            "volume", "--stream",     kMusicStream, "--set", value,
            "||",     "cmd",          "media_session", "volume", "--stream",
            kMusicStream, "--set",    value};
}

std::optional<StreamVolume> parseStreamVolume(std::string_view output) {
    constexpr std::string_view kStart = "volume is ";
    for (size_t at = output.find(kStart); at != std::string_view::npos;
         at = output.find(kStart, at + 1)) {
        size_t pos = at + kStart.size();
        StreamVolume volume;
        if (readInt(output, &pos, &volume.index) &&
            readText(output, &pos, " in range [") &&
            readInt(output, &pos, &volume.min) &&
            readText(output, &pos, "..") &&
            readInt(output, &pos, &volume.max) && readText(output, &pos, "]") &&
            volume.min < volume.max && volume.index >= volume.min &&
            volume.index <= volume.max) {
            return volume;
        }
    }
    return std::nullopt;
}

int unmuteIndex(int remembered, int min, int max) {
    if (remembered > min && remembered <= max) {
        return remembered;
    }
    return std::max(min + 1, (min + max + 1) / 2);
}

std::optional<int> VolumeSender::request(int index) {
    if (mBusy) {
        mWaiting = index;
        return std::nullopt;
    }
    mBusy = true;
    mSent = index;
    mWaiting.reset();
    return index;
}

std::optional<int> VolumeSender::finished() {
    std::optional<int> next;
    if (mWaiting && *mWaiting != mSent) {
        next = mWaiting;
    }
    mWaiting.reset();
    if (next) {
        mSent = *next;
        return next;
    }
    mBusy = false;
    return std::nullopt;
}

}  // namespace lean_title_bar
}  // namespace android
