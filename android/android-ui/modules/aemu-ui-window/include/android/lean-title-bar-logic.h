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

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the lean window's title bar shows and how its volume control talks to
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

// The left edge of a popup |popupWidth| wide that opens under a button whose
// left edge is at |anchorLeft|: aligned with the button, moved left so that it
// ends at |screenRight| when it would not fit, but never left of |screenLeft|.
int popupLeft(int anchorLeft, int popupWidth, int screenLeft, int screenRight);

// The guest's media volume (STREAM_MUSIC) as `cmd media_session volume` gives
// it.
struct StreamVolume {
    int index = 0;
    int min = 0;
    int max = 0;

    bool muted() const { return index <= min; }
};

// The adb arguments that read and set the guest's media volume.
std::vector<std::string> getMusicVolumeArgs();
std::vector<std::string> setMusicVolumeArgs(int index);

// Reads "volume is 5 in range [0..15]" from the output of
// getMusicVolumeArgs(), which has other lines around it ("[V] Connected to
// AudioService"). Nothing when no line has it or it does not add up.
std::optional<StreamVolume> parseStreamVolume(std::string_view output);

// The index the speaker button unmutes to: the last one above |min| it saw
// (|remembered|) when that is in range, else half way up.
int unmuteIndex(int remembered, int min, int max);

// Sends the slider's volume to the guest one adb command at a time: while one
// runs, only the newest value waits, so a drag does not start an adb process
// for every pixel and the guest ends at where the slider stopped.
class VolumeSender {
public:
    // The slider is at |index|. Returns the index to send now, or nothing if a
    // command runs (|index| then waits for it).
    std::optional<int> request(int index);
    // The running command finished. Returns the waiting index to send now, if
    // it differs from the one just sent.
    std::optional<int> finished();
    bool busy() const { return mBusy; }

private:
    bool mBusy = false;
    int mSent = 0;
    std::optional<int> mWaiting;
};

}  // namespace lean_title_bar
}  // namespace android
