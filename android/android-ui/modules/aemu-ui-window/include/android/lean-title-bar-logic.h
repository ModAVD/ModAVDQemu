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
#pragma once

#include <string>
#include <vector>

// What the lean window's title bar shows and what its volume button asks of
// the guest (ModAVD#231 screenshot, ModAVD#226 volume), without any windowing
// system, so that it can be tested on its own.
namespace android {
namespace lean_title_bar {

// The title bar's buttons, left to right, after the icon and the title.
enum class Item {
    Screenshot,
    Volume,
    Back,
    Home,
    Divider,
    Minimize,
    Maximize,
    Close,
};

// The title bar's items in order: screenshot, then volume when it is shown,
// right next to back, then home, a divider and the window buttons.
std::vector<Item> items(bool volumeShown);

// Whether the title bar has the volume button: only while the emulator plays
// sound, that is neither -audio none nor -no-audio / -noaudio. |audioBackend|
// is the -audio value, null when not given.
bool volumeButtonShown(const char* audioBackend, bool noAudio);

// Whether ANDROID_ADB_SERVER_PORT (|value|, null when not set) names an adb
// server the volume control may talk to: a decimal port in 1..65535 other than
// adb's default 5037, which the emulator must neither use nor start.
bool adbServerPortUsable(const char* value);

// The adb arguments that bring up Android's own volume panel for the media
// stream (STREAM_MUSIC) without changing the volume, as the volume button's
// click does.
std::vector<std::string> showVolumePanelArgs();

}  // namespace lean_title_bar
}  // namespace android
