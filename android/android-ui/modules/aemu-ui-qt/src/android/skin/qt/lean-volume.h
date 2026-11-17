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

#include <QFrame>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "android/lean-title-bar-logic.h"

class QHideEvent;
class QLabel;
class QMouseEvent;
class QPushButton;
class QShowEvent;
class QSlider;
class LeanTitleBar;

namespace android {
namespace emulation {
class AdbInterface;
}  // namespace emulation
}  // namespace android

// The bar the lean title bar's volume button opens under itself (ModAVD#226),
// as MuMu has it: a speaker that mutes and unmutes on the left, a slider on
// the right. A click anywhere outside it, the volume button included, closes
// it.
class LeanVolumePopup : public QFrame {
    Q_OBJECT

public:
    explicit LeanVolumePopup(QWidget* parent);

    // Opens under |anchor|, the volume button, with the slider disabled until
    // showVolume() or showUnavailable().
    void openUnder(QPushButton* anchor);
    void showVolume(const android::lean_title_bar::StreamVolume& volume);
    // Shows the speaker as muted or not, the slider where it is.
    void showMuted(bool muted);
    void setSliderValue(int index);
    void showUnavailable(const QString& text);

signals:
    void volumeDragged(int index);
    void muteClicked();
    void closed();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QPointer<QPushButton> mAnchor;
    QPushButton* mSpeaker = nullptr;
    QSlider* mSlider = nullptr;
    QLabel* mStatus = nullptr;
};

// The volume button's control: reads the guest's media volume when the bar
// opens and sets it while the slider moves, over adb (`cmd media_session
// volume --stream 3`). adb runs on the AdbInterface's own thread and answers
// on the UI thread; the slider sends one command at a time and only its
// newest value waits. It talks only to the adb server that
// ANDROID_ADB_SERVER_PORT names (the manager's own), never to 5037: without
// one, or without adb, the button is disabled and says why.
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
    void toggle();
    void readVolume();
    void onDragged(int index);
    void onMuteClicked();
    void send(std::optional<int> index);
    void runAdb(const std::vector<std::string>& args,
                bool wantOutput,
                std::function<void(bool ok, const std::string& output)> done);

    LeanTitleBar* mTitleBar;
    AdbGetter mAdb;
    LeanVolumePopup* mPopup = nullptr;
    android::lean_title_bar::VolumeSender mSender;
    std::optional<android::lean_title_bar::StreamVolume> mVolume;
    // The last index above the minimum, for unmuting.
    int mRemembered = -1;
    // Bumped whenever the bar opens, so a late answer to an older read is
    // dropped.
    int mReadGeneration = 0;
};
