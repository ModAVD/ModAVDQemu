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
#include "android/skin/qt/lean-volume.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QLabel>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPushButton>
#include <QRect>
#include <QScreen>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSize>
#include <QSlider>

#include <sstream>
#include <utility>

#include "android/base/system/System.h"
#include "android/emulation/control/adb/AdbInterface.h"
#include "android/skin/qt/lean-title-bar.h"

namespace tb = android::lean_title_bar;
using android::emulation::AdbInterface;
using android::emulation::OptionalAdbCommandResult;

namespace {

// The lean title bar's palette (lean-title-bar.cpp).
constexpr char kStyle[] =
        "#leanVolumePopup { background: #20212D; border: 1px solid #3B3D52; }"
        "#leanVolumePopup QLabel { color: #E8E8EC; background: transparent; }"
        "#leanVolumePopup QPushButton { background: #20212D; border: 0; }"
        "#leanVolumePopup QPushButton:hover { background: #303142; }"
        "#leanVolumePopup QPushButton:pressed { background: #3B3D52; }"
        "#leanVolumePopup QSlider::groove:horizontal {"
        " height: 4px; background: #3B3D52; border-radius: 2px; }"
        "#leanVolumePopup QSlider::sub-page:horizontal {"
        " background: #E8E8EC; border-radius: 2px; }"
        "#leanVolumePopup QSlider::handle:horizontal {"
        " background: #E8E8EC; width: 12px; margin: -4px 0; border-radius: 6px; }"
        "#leanVolumePopup QSlider::sub-page:horizontal:disabled {"
        " background: #3B3D52; }"
        "#leanVolumePopup QSlider::handle:horizontal:disabled {"
        " background: #5A5C70; }";

constexpr int kHeight = 40;
constexpr int kSpeakerSize = 32;
constexpr int kIconSize = 16;
constexpr int kSliderWidth = 160;
constexpr int kAdbTimeoutMs = 5000;

}  // namespace

LeanVolumePopup::LeanVolumePopup(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint |
                             Qt::NoDropShadowWindowHint) {
    setObjectName(QStringLiteral("leanVolumePopup"));
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QString::fromLatin1(kStyle));
    setFixedHeight(kHeight);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 0, 12, 0);
    layout->setSpacing(8);

    mSpeaker = new QPushButton(this);
    mSpeaker->setFocusPolicy(Qt::NoFocus);
    mSpeaker->setFlat(true);
    mSpeaker->setFixedSize(kSpeakerSize, kSpeakerSize);
    mSpeaker->setIconSize(QSize(kIconSize, kIconSize));
    layout->addWidget(mSpeaker, 0, Qt::AlignVCenter);

    mSlider = new QSlider(Qt::Horizontal, this);
    mSlider->setFocusPolicy(Qt::NoFocus);
    mSlider->setFixedWidth(kSliderWidth);
    mSlider->setTracking(true);
    mSlider->setAccessibleName(QString::fromUtf8("媒体音量"));
    layout->addWidget(mSlider, 0, Qt::AlignVCenter);

    mStatus = new QLabel(this);
    mStatus->setFixedWidth(kSliderWidth);
    mStatus->hide();
    layout->addWidget(mStatus, 0, Qt::AlignVCenter);

    connect(mSpeaker, &QPushButton::clicked, this,
            &LeanVolumePopup::muteClicked);
    connect(mSlider, &QSlider::valueChanged, this,
            &LeanVolumePopup::volumeDragged);
    showMuted(false);
    adjustSize();
}

void LeanVolumePopup::openUnder(QPushButton* anchor) {
    mAnchor = anchor;
    mStatus->hide();
    mSlider->show();
    mSlider->setEnabled(false);
    mSpeaker->setEnabled(false);
    adjustSize();

    const QPoint below = anchor->mapToGlobal(QPoint(0, anchor->height()));
    int left = below.x();
    if (QScreen* screen = anchor->screen()) {
        const QRect area = screen->availableGeometry();
        left = tb::popupLeft(below.x(), width(), area.left(),
                             area.left() + area.width());
    }
    move(left, below.y());
    show();
    raise();
}

void LeanVolumePopup::showVolume(const tb::StreamVolume& volume) {
    mStatus->hide();
    mSlider->show();
    const QSignalBlocker blocker(mSlider);
    mSlider->setRange(volume.min, volume.max);
    mSlider->setPageStep(1);
    mSlider->setValue(volume.index);
    mSlider->setEnabled(true);
    mSpeaker->setEnabled(true);
    showMuted(volume.muted());
}

void LeanVolumePopup::showMuted(bool muted) {
    mSpeaker->setIcon(QIcon(muted ? QStringLiteral(":/dark/volume_off_lean")
                                  : QStringLiteral(":/dark/volume_up")));
    const QString tip =
            muted ? QString::fromUtf8("取消静音") : QString::fromUtf8("静音");
    mSpeaker->setToolTip(tip);
    mSpeaker->setAccessibleName(tip);
}

void LeanVolumePopup::setSliderValue(int index) {
    const QSignalBlocker blocker(mSlider);
    mSlider->setValue(index);
}

void LeanVolumePopup::showUnavailable(const QString& text) {
    mSlider->hide();
    mSpeaker->setEnabled(false);
    mStatus->setText(text);
    mStatus->show();
}

void LeanVolumePopup::mousePressEvent(QMouseEvent* event) {
    if (!rect().contains(event->position().toPoint())) {
        // A click on the volume button closes the bar and goes no further,
        // or the button would open it again at once.
        if (mAnchor && mAnchor->isVisible() &&
            QRect(mAnchor->mapToGlobal(QPoint(0, 0)), mAnchor->size())
                    .contains(event->globalPosition().toPoint())) {
            setAttribute(Qt::WA_NoMouseReplay, true);
        }
        close();
        return;
    }
    QFrame::mousePressEvent(event);
}

void LeanVolumePopup::showEvent(QShowEvent* event) {
    setAttribute(Qt::WA_NoMouseReplay, false);
    QFrame::showEvent(event);
}

void LeanVolumePopup::hideEvent(QHideEvent* event) {
    QFrame::hideEvent(event);
    emit closed();
}

LeanVolumeControl::LeanVolumeControl(LeanTitleBar* titleBar, AdbGetter adb)
    : QObject(titleBar), mTitleBar(titleBar), mAdb(std::move(adb)) {
    QPushButton* button = mTitleBar->volumeButton();
    mPopup = new LeanVolumePopup(button->window());
    connect(button, &QPushButton::clicked, this, &LeanVolumeControl::toggle);
    connect(mPopup, &LeanVolumePopup::volumeDragged, this,
            &LeanVolumeControl::onDragged);
    connect(mPopup, &LeanVolumePopup::muteClicked, this,
            &LeanVolumeControl::onMuteClicked);
    connect(mTitleBar, &LeanTitleBar::buttonsDisabled, mPopup,
            &QWidget::close);

    const std::string port =
            android::base::System::get()->envGet("ANDROID_ADB_SERVER_PORT");
    if (!tb::adbServerPortUsable(port.empty() ? nullptr : port.c_str())) {
        mTitleBar->setVolumeAvailable(
                false, QString::fromUtf8("音量：没有可用的 adb 服务，调不了"));
    }
}

void LeanVolumeControl::checkAvailable() {
    if (!mTitleBar->volumeAvailable()) {
        return;
    }
    AdbInterface* adb = mAdb ? mAdb() : nullptr;
    if (!adb || adb->detectedAdbPath().empty()) {
        mTitleBar->setVolumeAvailable(
                false, QString::fromUtf8("音量：没找到 adb，调不了"));
    }
}

void LeanVolumeControl::toggle() {
    if (mPopup->isVisible()) {
        mPopup->close();
        return;
    }
    mPopup->openUnder(mTitleBar->volumeButton());
    readVolume();
}

void LeanVolumeControl::readVolume() {
    const int generation = ++mReadGeneration;
    mVolume.reset();
    runAdb(tb::getMusicVolumeArgs(), true,
           [this, generation](bool ok, const std::string& output) {
               if (generation != mReadGeneration || !mPopup->isVisible()) {
                   return;
               }
               const auto volume =
                       ok ? tb::parseStreamVolume(output) : std::nullopt;
               if (!volume) {
                   mPopup->showUnavailable(
                           QString::fromUtf8("读不到安卓的音量"));
                   return;
               }
               mVolume = volume;
               if (!volume->muted()) {
                   mRemembered = volume->index;
               }
               mPopup->showVolume(*volume);
           });
}

void LeanVolumeControl::onDragged(int index) {
    if (!mVolume) {
        return;
    }
    mVolume->index = index;
    if (!mVolume->muted()) {
        mRemembered = index;
    }
    mPopup->showMuted(mVolume->muted());
    send(mSender.request(index));
}

void LeanVolumeControl::onMuteClicked() {
    if (!mVolume) {
        return;
    }
    // Muting sets the minimum and unmuting the last index above it: setting
    // a stream's volume above zero unmutes it, and the guest keeps the index
    // across boots.
    const int index =
            mVolume->muted()
                    ? tb::unmuteIndex(mRemembered, mVolume->min, mVolume->max)
                    : mVolume->min;
    mVolume->index = index;
    mPopup->setSliderValue(index);
    mPopup->showMuted(mVolume->muted());
    send(mSender.request(index));
}

void LeanVolumeControl::send(std::optional<int> index) {
    if (!index) {
        return;
    }
    runAdb(tb::setMusicVolumeArgs(*index), false,
           [this](bool, const std::string&) { send(mSender.finished()); });
}

void LeanVolumeControl::runAdb(
        const std::vector<std::string>& args,
        bool wantOutput,
        std::function<void(bool ok, const std::string& output)> done) {
    // The answer comes back on the UI thread, and only while this lives.
    QPointer<LeanVolumeControl> self(this);
    auto answer = [self, done = std::move(done)](bool ok, std::string output) {
        QMetaObject::invokeMethod(
                qApp,
                [self, done, ok, output = std::move(output)] {
                    if (self) {
                        done(ok, output);
                    }
                },
                Qt::QueuedConnection);
    };
    AdbInterface* adb = mAdb ? mAdb() : nullptr;
    if (!adb) {
        answer(false, std::string());
        return;
    }
    auto command = adb->runAdbCommand(
            args,
            [answer, wantOutput](const OptionalAdbCommandResult& result) {
                const bool ok = result && result->exit_code == 0;
                std::string output;
                if (ok && wantOutput && result->output) {
                    std::ostringstream text;
                    text << result->output->rdbuf();
                    output = text.str();
                }
                answer(ok, std::move(output));
            },
            kAdbTimeoutMs, wantOutput);
    if (!command) {
        // Not running Android, so no adb.
        answer(false, std::string());
    }
}
