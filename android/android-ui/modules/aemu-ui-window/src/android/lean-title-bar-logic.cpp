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

#include <cstring>

namespace android {
namespace lean_title_bar {

namespace {

// adb's own default server port.
constexpr long kDefaultAdbServerPort = 5037;
// AudioSystem.STREAM_MUSIC.
constexpr char kMusicStream[] = "3";

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

// `cmd media_session volume --adj same` adjusts by nothing and, with --show,
// brings up the system's volume panel, as a volume key does. On API 34 the
// shell's own `cmd media_session volume` is turned down by AppOps
// (com.android.server.media from uid 2000 may not AUDIO_MEDIA_VOLUME), so it
// goes through su first and falls back to the shell only where there is no
// su. adb shell joins the words and runs them in the guest's sh, so the || is
// the guest's.
std::vector<std::string> showVolumePanelArgs() {
    return {"shell",
            "su", "0", "cmd", "media_session", "volume", "--show",
            "--stream", kMusicStream, "--adj", "same",
            "||",
            "cmd", "media_session", "volume", "--show",
            "--stream", kMusicStream, "--adj", "same"};
}

}  // namespace lean_title_bar
}  // namespace android
