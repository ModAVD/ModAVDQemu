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

#include <QObject>

#include <functional>

class LeanTitleBar;

namespace android {
namespace emulation {
class AdbInterface;
}  // namespace emulation
}  // namespace android

// The volume button's control (ModAVD#226): a click brings up Android's own
// volume panel for the media stream, as a volume key does, over adb (`cmd
// media_session volume --show --adj same`). adb runs on the AdbInterface's own
// thread, so the window does not wait for it. It talks only to the adb server
// that ANDROID_ADB_SERVER_PORT names (the manager's own), never to 5037:
// without one, or without adb, the button is disabled and says why.
class LeanVolumeControl : public QObject {
    Q_OBJECT

public:
    using AdbGetter = std::function<android::emulation::AdbInterface*()>;

    LeanVolumeControl(LeanTitleBar* titleBar, AdbGetter adb);

    // Disables the button when there is no adb or no adb server of the
    // manager's to talk to. Asks the AdbInterface, so it is called once the
    // window is up rather than while it is made.
    void checkAvailable();

private:
    void showPanel();

    LeanTitleBar* mTitleBar;
    AdbGetter mAdb;
};
