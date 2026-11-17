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

#include <QPushButton>
#include <QString>

#include <string>
#include <utility>

#include "android/base/system/System.h"
#include "android/emulation/control/adb/AdbInterface.h"
#include "android/lean-title-bar-logic.h"
#include "android/skin/qt/lean-title-bar.h"

namespace tb = android::lean_title_bar;
using android::emulation::AdbInterface;
using android::emulation::OptionalAdbCommandResult;

namespace {

constexpr int kAdbTimeoutMs = 5000;

}  // namespace

LeanVolumeControl::LeanVolumeControl(LeanTitleBar* titleBar, AdbGetter adb)
    : QObject(titleBar), mTitleBar(titleBar), mAdb(std::move(adb)) {
    connect(mTitleBar->volumeButton(), &QPushButton::clicked, this,
            &LeanVolumeControl::showPanel);

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

void LeanVolumeControl::showPanel() {
    AdbInterface* adb = mAdb ? mAdb() : nullptr;
    if (!adb) {
        return;
    }
    // The panel is the answer; the command's own result says nothing more.
    adb->runAdbCommand(tb::showVolumePanelArgs(),
                       [](const OptionalAdbCommandResult&) {}, kAdbTimeoutMs,
                       false);
}
