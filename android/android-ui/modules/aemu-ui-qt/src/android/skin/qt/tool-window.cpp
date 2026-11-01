/* Copyright (C) 2015 The Android Open Source Project
 **
 ** This software is licensed under the terms of the GNU General Public
 ** License version 2, as published by the Free Software Foundation, and
 ** may be copied, distributed, and modified under those terms.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 */

#include "android/skin/qt/tool-window.h"
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <istream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

#include <QAbstractButton>
#include <QApplication>
#include <QBackingStore>
#include <QBitmap>
#include <QByteArray>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFlags>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QPushButton>
#include <QRect>
#include <QScrollArea>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QString>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QVector>
#include <QWidget>
#include <QWindow>
#include <QtCore>
#include <thread>

#include "absl/time/time.h"
#include "aemu/base/EventNotificationSupport.h"
#include "aemu/base/Stopwatch.h"
#include "aemu/base/logging/Log.h"
#include "aemu/base/logging/LogSeverity.h"
#include "android/avd/info.h"
#include "android/avd/util.h"
#include "android/base/system/System.h"
#include "android/console.h"
#include "android/emulation/ConfigDirs.h"
#include "android/emulation/control/adb/AdbInterface.h"
#include "android/emulation/control/clipboard_agent.h"
#include "android/emulation/control/sensors_agent.h"
#include "android/emulation/control/utils/TypeConversion.h"
#include "android/emulator-window.h"
#include "android/hw-events.h"
#include "android/hw-sensors.h"
#include "android/metrics/MetricsReporter.h"
#include "android/metrics/MetricsWriter.h"
#include "android/metrics/studio_stats_wrapper.pb.h"
#include "android/physics/Physics.h"
#include "android/skin/android_keycodes.h"
#include "android/skin/event.h"
#include "android/skin/linux_keycodes.h"
#include "android/skin/qt/emulator-qt-window.h"
#include "android/skin/qt/extended-pages/common.h"
#include "android/skin/qt/extended-pages/virtual-sensors-page.h"
#include "android/skin/qt/extended-window.h"
#include "android/skin/qt/posture-selection-dialog.h"
#include "android/skin/qt/qt-settings.h"
#include "android/skin/qt/qt-ui-commands.h"
#include "android/skin/qt/stylesheet.h"
#include "android/skin/qt/touchpad-window.h"
#include "android/skin/qt/virtualscene-control-window.h"
#include "android/skin/qt/xr-environment-mode-dialog.h"
#include "android/skin/qt/xr-input-mode-dialog.h"
#include "android/ui-emu-agent.h"
#include "android/utils/debug.h"
#include "android/utils/system.h"
#include "android/xr/XrService.h"
#include "host-common/FeatureControl.h"
#include "host-common/Features.h"
#include "host-common/hw-config-helper.h"
#include "host-common/opengles.h"
#include "host-common/snapshot_common.h"
#include "host-common/snapshot_interface.h"
#include "host-common/window_agent.h"
#include "ui_tools.h"
#include "xr_emulator_conn.pb.h"

#ifdef __APPLE__
#include "android/skin/qt/mac-native-window.h"  // for getNSWindow, nsW...
#endif

namespace {

struct GestureData {
    const char* button_name;
    const char* button_tip;
    xr_emulator_proto::HandGesture protoId;
};

// To rotate a hand, press Cmd key on Mac or Ctrl key on Win&Linux, plus mouse move.
#ifdef __APPLE__
#define C_KEY_NAME "Cmd"
#else
#define C_KEY_NAME "Ctrl"
#endif

// clang-format off

#define TABLE_ROW(column1, column2) \
    "<tr><th align=\"left\">" column1 "</th><td>" column2 "</td></tr>"

// HTML text of help that will be shown in the tooltip of hand buttons
// so that user knows how to manipulate the hand.
#define HAND_HELP \
    "<br>Manipulate hand with keyboard/mouse:\
    <table>"\
        TABLE_ROW("Q/E", "Move up/down")\
        TABLE_ROW("A/D", "Move left/right")\
        TABLE_ROW("W/S", "Move forward/back")\
        TABLE_ROW("Mouse Button Press", "Start gesture")\
        TABLE_ROW("Mouse Button Release", "End gesture")\
        TABLE_ROW(C_KEY_NAME "+Mouse Move", "Rotate hand")\
    "</table>"

#define HAND_BUTTON_TOOLTIP(base) \
    ("<html>" base HAND_HELP "</html>")

// clang-format on

const std::map<QString, GestureData> LEFT_HAND_GESTURE_DATA = {
        {Ui::GESTURE_NAME_PINCH,
         {"xr_left_hand_pinch", HAND_BUTTON_TOOLTIP("Left hand: Pinch"),
          xr_emulator_proto::LEFT_HAND_PINCH}},
        {Ui::GESTURE_NAME_GRAB,
         {"xr_left_hand_grab", HAND_BUTTON_TOOLTIP("Left hand: Grab"),
          xr_emulator_proto::LEFT_HAND_GRAB}},
        {Ui::GESTURE_NAME_POKE,
         {"xr_left_hand_poke", HAND_BUTTON_TOOLTIP("Left hand: Poke"),
          xr_emulator_proto::LEFT_HAND_POKE}},
};

const std::map<QString, GestureData> RIGHT_HAND_GESTURE_DATA = {
        {Ui::GESTURE_NAME_PINCH,
         {"xr_right_hand_pinch", HAND_BUTTON_TOOLTIP("Right hand: Pinch"),
          xr_emulator_proto::RIGHT_HAND_PINCH}},
        {Ui::GESTURE_NAME_GRAB,
         {"xr_right_hand_grab", HAND_BUTTON_TOOLTIP("Right hand: Grab"),
          xr_emulator_proto::RIGHT_HAND_GRAB}},
        {Ui::GESTURE_NAME_POKE,
         {"xr_right_hand_poke", HAND_BUTTON_TOOLTIP("Right hand: Poke"),
          xr_emulator_proto::RIGHT_HAND_POKE}},
};

void ChangeIcon(QPushButton* button, const char* icon, const char* tip) {
    button->setIcon(getIconForCurrentTheme(icon));
    button->setProperty("themeIconName", icon);
    button->setProperty("toolTip", tip);
}

void setRelativeMouseMode(bool relative) {
    const auto emulatorWindow = EmulatorQtWindow::getInstance();
    if (emulatorWindow) {
        emulatorWindow->setRelativeMouseCoordMode(relative);
    } else {
        LOG(WARNING) << "No window found to set mouse coordinates mode";
    }
}

}  // namespace

using android::base::System;
using android::base::WorkerProcessingResult;
using android::emulation::OptionalAdbCommandResult;
using android::metrics::MetricsReporter;
using Ui::Settings::SaveSnapshotOnExit;

namespace proto = android_studio;
namespace fc = android::featurecontrol;
namespace xr_service = android::xr::xr_service;

// Instances here run unattended for automation, so the side toolbar keeps only
// minimize, close, back and home. ANDROID_EMU_FULL_TOOLBAR=1 shows the full
// toolbar again.
static bool leanToolbar() {
    static const bool lean =
            System::get()->getEnvironmentVariable("ANDROID_EMU_FULL_TOOLBAR") !=
            "1";
    return lean;
}

bool ToolWindow::inWindowNav() {
#ifdef _WIN32
    return leanToolbar() && !getConsoleAgents()->settings->android_cmdLineOptions()->qt_hide_window;
#else
    return false;
#endif
}

template <typename T>
ToolWindow::WindowHolder<T>::WindowHolder(ToolWindow* tw,
                                          OnCreatedCallback onCreated)
    : mWindow(new T(tw->mEmulatorWindow, tw)) {
    (tw->*onCreated)(mWindow);
}

template <typename T>
ToolWindow::WindowHolder<T>::~WindowHolder() {
    // The window may have slots with subscribers, so use deleteLater() instead
    // of a regular delete for it.
    mWindow->deleteLater();
}

ToolWindow::ExtendedWindowHolder::ExtendedWindowHolder(
        ToolWindow* tw,
        OnCreatedCallback onCreated) {
    mWindow = new ExtendedWindow(tw->mEmulatorWindow, tw);
    (tw->*onCreated)(mWindow);
}

const UiEmuAgent* ToolWindow::sUiEmuAgent = nullptr;
static ToolWindow* sToolWindow = nullptr;

ToolWindow::ToolWindow(EmulatorQtWindow* window,
                       QWidget* parent,
                       ToolWindow::UIEventRecorderPtr event_recorder,
                       ToolWindow::UserActionsCounterPtr user_actions_counter)
    : QFrame(parent),
      mEmulatorWindow(window),
      mExtendedWindow(this, &ToolWindow ::onExtendedWindowCreated),
      mVirtualSceneControlWindow(this,
                                 &ToolWindow::onVirtualSceneWindowCreated),
      mTouchpadWindow(this, &ToolWindow::onTouchpadWindowCreated),
      mToolsUi(new Ui::ToolControls),
      mUIEventRecorder(event_recorder),
      mUserActionsCounter(user_actions_counter),
      mPostureSelectionDialog(new PostureSelectionDialog(this)),
      mResizableDialog(new ResizableDialog(this)),
      mXrEnvironmentModeDialog(new XrEnvironmentModeDialog(this)),
      mXrInputModeDialog(new XrInputModeDialog(this)),
      mLeftHandDialog(new LeftHandDialog(this)),
      mRightHandDialog(new RightHandDialog(this)),
      mFoldableSyncToAndroidSuccess(false),
      mFoldableSyncToAndroidTimeout(false),
      mFoldableSyncToAndroid([this](FoldableSyncToAndroidItem&& item) {
          return foldableSyncToAndroidItemFunction(item);
      }) {
// "Tool" type windows live in another layer on top of everything in OSX, which
// is undesirable because it means the extended window must be on top of the
// emulator window. However, on Windows and Linux, "Tool" type windows are the
// only way to make a window that does not have its own taskbar item.
#ifdef __APPLE__
    Qt::WindowFlags flag = Qt::Dialog;
#else
    Qt::WindowFlags flag = Qt::Tool;
#endif
    setWindowFlags(flag | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    mToolsUi->setupUi(this);

    const AvdInfo* avdInfo = getConsoleAgents()->settings->avdInfo();
    const AvdFlavor avdFlavor = avdInfo ? avdInfo_getAvdFlavor(avdInfo) : AVD_OTHER;
    bool avdIsDesktopApi36OrHigher = avdInfo && avdInfo_isDesktopApi36OrHigher(avdInfo);

    mToolsUi->mainLayout->setAlignment(Qt::AlignCenter);
    mToolsUi->winButtonsLayout->setAlignment(Qt::AlignCenter);
    mToolsUi->controlsLayout->setAlignment(Qt::AlignCenter);
    if (android_foldable_any_folded_area_configured() ||
        android_foldable_hinge_configured() || resizableEnabled() ||
        avdFlavor == AVD_XR) {
        mToolsUi->zoom_button->hide();
        mToolsUi->zoom_button->setEnabled(false);
    }
    // Get the latest user selections from the user-config code.
    SettingsTheme theme = getSelectedTheme();
    adjustAllButtonsForTheme(theme);
    updateTheme(Ui::stylesheetForTheme(theme));

    QString default_shortcuts =
            "Ctrl+Shift+U SHOW_PANE_BUGREPORT\n"
            "Ctrl+Shift+M SHOW_PANE_MICROPHONE\n"
            "Ctrl+Shift+N SHOW_PANE_SNAPSHOT\n"
            "Ctrl+Shift+S SHOW_PANE_SETTINGS\n"
#ifdef __APPLE__
            "Ctrl+/     SHOW_PANE_HELP\n"
#else
            "F1         SHOW_PANE_HELP\n"
#endif
            ;

    if (!avdIsDesktopApi36OrHigher) {
        default_shortcuts +=
            "Ctrl+S     TAKE_SCREENSHOT\n"
            "Ctrl+P     POWER\n"
#ifndef __APPLE__
            "Ctrl+H     HOME\n"
#else
            "Ctrl+Shift+H  HOME\n"
#endif
            "Ctrl+O     OVERVIEW\n"
            "Ctrl+Backspace BACK\n";
    }

    if (avdFlavor != AVD_XR && !avdIsDesktopApi36OrHigher) {
        default_shortcuts +=
            "Ctrl+Shift+A       SHOW_PANE_CAMERA\n"
            "Ctrl+M             MENU\n"
            "Ctrl+Shift+Up      PAN_UP\n"
            "Ctrl+Shift+Down    PAN_DOWN\n"
            "Ctrl+Shift+Left    PAN_LEFT\n"
            "Ctrl+Shift+Right   PAN_RIGHT\n"
            "Ctrl+T             TOGGLE_TRACKBALL\n";
    }

    if (avdFlavor != AVD_WEAR && !avdIsDesktopApi36OrHigher) {
        default_shortcuts += "Ctrl+=     VOLUME_UP\n";
        default_shortcuts += "Ctrl+-     VOLUME_DOWN\n";
        if (avdFlavor != AVD_XR) {
            default_shortcuts += "Ctrl+Left  ROTATE_LEFT\n";
            default_shortcuts += "Ctrl+Right ROTATE_RIGHT\n";
        }
    }

    if (!android_foldable_any_folded_area_configured() &&
        !android_foldable_hinge_configured() &&
        !android_foldable_rollable_configured() && !resizableEnabled() &&
        avdFlavor != AVD_XR && !avdIsDesktopApi36OrHigher) {
        // Zoom is not available for foldable and resizable AVDs
        default_shortcuts += "Ctrl+Z    ENTER_ZOOM\n";
        default_shortcuts += "Ctrl+Up   ZOOM_IN\n";
        default_shortcuts += "Ctrl+Down ZOOM_OUT\n";
    }
    if (fc::isEnabled(fc::PlayStoreImage) &&
        getConsoleAgents()->settings->hw()->PlayStore_enabled) {
        default_shortcuts += "Ctrl+Shift+G SHOW_PANE_GPLAY\n";
    }
    if (fc::isEnabled(fc::ScreenRecording)) {
        default_shortcuts += "Ctrl+Shift+R SHOW_PANE_RECORD\n";
    }

    if (avdInfo) {
        if (avdFlavor == AVD_WEAR) {
            if (avdInfo_getApiLevel(avdInfo) >= 28) {
                default_shortcuts += "Ctrl+Shift+O WEAR_1\n";
                default_shortcuts += "Ctrl+Shift+T TILT\n";
                default_shortcuts += "Ctrl+Shift+E PALM\n";
            }
            if (avdInfo_getApiLevel(avdInfo) > 28) {
                default_shortcuts += "Ctrl+Shift+I WEAR_2\n";
            }
        } else if (avdFlavor == AVD_ANDROID_AUTO) {
            default_shortcuts += "Ctrl+Shift+T SHOW_PANE_CAR\n";
            default_shortcuts += "Ctrl+Shift+O SHOW_PANE_CAR_ROTARY\n";
            default_shortcuts += "Ctrl+Shift+I SHOW_PANE_SENSOR_REPLAY\n";
        } else if (avdFlavor == AVD_XR) {
            default_shortcuts +=
                "Ctrl+Shift+.   XR_SCREEN_RECENTER\n";
        } else if (fc::isEnabled(fc::MultiDisplay) &&
                   !android_foldable_any_folded_area_configured() &&
                   !android_foldable_hinge_configured() &&
                   !android_foldable_rollable_configured() &&
                   !resizableEnabled()) {
            // Multi display pane should not be available on cars.
            // Multi display pane should not be available on foldable
            // or resizable AVDs
            default_shortcuts += "Ctrl+Shift+M SHOW_PANE_MULTIDISPLAY\n";
        }
    }

    if (fc::isEnabled(fc::TvRemote) && avdFlavor == AVD_TV) {
        default_shortcuts += "Ctrl+Shift+D SHOW_PANE_TV_REMOTE\n";
    } else if (avdFlavor != AVD_XR && !avdIsDesktopApi36OrHigher) {
        default_shortcuts += "Ctrl+Shift+D SHOW_PANE_DPAD\n";
    }

    if (fc::isEnabled(fc::AllAppsForHomeTray)) {
       mToolsUi->home_button->setHidden(true);
    } else {
       mToolsUi->all_apps_button->setHidden(true);
    }

    if (!fc::isEnabled(fc::MicrophoneToggleUI)) {
       mToolsUi->microphone_button->setHidden(true);
    }

    connect(this, SIGNAL(_externalMicrophoneEnabledChanged(bool)), this,
            SLOT(onExternalMicrophoneEnabledChanged(bool)));

    auto notifier = static_cast<android::base::EventNotificationSupport<bool>*>(
            getConsoleAgents()->vm->getRealAudioEventListener());
    if (notifier) {
        mRealAudioEventListener =
                std::make_unique<android::base::RaiiEventListener<
                        android::base::EventNotificationSupport<bool>, bool>>(
                        notifier, [this](bool enabled) {
                            emit this->_externalMicrophoneEnabledChanged(
                                    enabled);
                        });
    }

    updateMicrophoneUI(getMicrophoneEnabled());

    if (avdFlavor != AVD_TV) {
        if (avdFlavor != AVD_XR && !avdIsDesktopApi36OrHigher) {
            default_shortcuts +=
                    "Ctrl+Shift+L SHOW_PANE_LOCATION\n"
                    "Ctrl+Shift+C SHOW_PANE_CELLULAR\n"
                    "Ctrl+Shift+P SHOW_PANE_PHONE\n"
                    "Ctrl+Shift+V SHOW_PANE_VIRTSENSORS\n"
                    "Ctrl+Shift+F SHOW_PANE_FINGER\n";
        }
        default_shortcuts +=
            "Ctrl+Shift+B   SHOW_PANE_BATTERY\n";
    }

    QTextStream stream(&default_shortcuts);
    mShortcutKeyStore.populateFromTextStream(stream, parseQtUICommand);

    if (avdFlavor != AVD_XR && !avdIsDesktopApi36OrHigher) {
        // Multitouch is disabled on XR images.
        // Need to add this one separately because QKeySequence cannot parse
        // the string "Ctrl".
        mShortcutKeyStore.add(QKeySequence(Qt::Key_Control | Qt::ControlModifier),
                          QtUICommand::SHOW_MULTITOUCH);

        // Virtual Scene control shortcuts are disabled on XR images since they
        // conflict with the navigation shortcuts.
        VirtualSceneControlWindow::addShortcutKeysToKeyStore(mShortcutKeyStore);
    }

    // Update tool tips on all push buttons.
    const QList<QPushButton*> childButtons =
            findChildren<QPushButton*>(QString(), Qt::FindDirectChildrenOnly);
    for (auto button : childButtons) {
        QVariant uiCommand = button->property("uiCommand");
        if (uiCommand.isValid()) {
            QtUICommand cmd;
            if (parseQtUICommand(uiCommand.toString(), &cmd)) {
                QVector<QKeySequence>* shortcuts =
                        mShortcutKeyStore.reverseLookup(cmd);
                if (shortcuts && shortcuts->length() > 0) {
                    button->setToolTip(getQtUICommandDescription(cmd) + " (" +
                                       shortcuts->at(0).toString(
                                               QKeySequence::NativeText) +
                                       ")");
                }
            }
        } else if (button != mToolsUi->close_button &&
                   button != mToolsUi->minimize_button &&
                   button != mToolsUi->more_button &&
                   button != mToolsUi->environment_button) {
            // Almost all toolbar buttons are required to have a uiCommand
            // property.
            // Unfortunately, we have no way of enforcing it at compile time.
        }

        // All XR buttons are expected to define the `xrButtonType` property.
        QVariant xrButtonTypeVariant = button->property("xrButtonType");
        if (xrButtonTypeVariant.isValid()) {
            std::string xrButtonType = xrButtonTypeVariant.toString().toStdString();
            if (mXrButtonTypeToPushButtonsMap.find(xrButtonType) ==
                        mXrButtonTypeToPushButtonsMap.end()) {
                mXrButtonTypeToPushButtonsMap[xrButtonType] =
                        {std::ref(*button)};
            } else {
                mXrButtonTypeToPushButtonsMap[xrButtonType].push_back(
                        std::ref(*button));
            }
        }
    }

    // update tooltips of these two buttons
    updateLeftHandButton(mCurrentLeftHandGesture);
    updateRightHandButton(mCurrentRightHandGesture);

    // Android doesn't support tablet mode now.
    mToolsUi->tablet_mode_button->setHidden(true);

    if (avdFlavor == AVD_TV) {
        // Android TV should not rotate
        // TODO: emulate VESA mounts for use with
        // vertically scrolling arcade games
        mToolsUi->prev_layout_button->setHidden(true);
        mToolsUi->next_layout_button->setHidden(true);
    }

    if (avdFlavor == AVD_WEAR) {
        // Wear OS shouldn't get rotate nor volume up/down buttons.
        mToolsUi->prev_layout_button->setHidden(true);
        mToolsUi->next_layout_button->setHidden(true);
        mToolsUi->volume_up_button->setHidden(true);
        mToolsUi->volume_down_button->setHidden(true);
    }

    if (avdFlavor == AVD_WEAR && avdInfo_getApiLevel(avdInfo) >= 28) {
        // Use new button layout for >= API 28 wear emulators
        mToolsUi->overview_button->setHidden(true);
        mToolsUi->power_button->setHidden(true);
        mToolsUi->home_button->setHidden(true);

        mToolsUi->controlsLayout->removeWidget(mToolsUi->back_button);
        mToolsUi->controlsLayout->insertWidget(0, mToolsUi->back_button);

        if (avdInfo_getApiLevel(avdInfo) == 28) {
            mToolsUi->wear_button_2->setHidden(true);
        }
    } else {
        mToolsUi->wear_button_1->setHidden(true);
        mToolsUi->wear_button_2->setHidden(true);
        mToolsUi->palm_button->setHidden(true);
        mToolsUi->tilt_button->setHidden(true);
    }

    if (avdFlavor == AVD_ANDROID_AUTO) {
        // Android Auto doesn't support rotate, home, back, recent
        mToolsUi->prev_layout_button->setHidden(true);
        mToolsUi->next_layout_button->setHidden(true);
        mToolsUi->back_button->setHidden(true);
        mToolsUi->overview_button->setHidden(true);
    }

    if (avdFlavor == AVD_DESKTOP) {
        // Desktop device does not rotate
        mToolsUi->prev_layout_button->setHidden(true);
        mToolsUi->next_layout_button->setHidden(true);
    }

    if (!avdIsDesktopApi36OrHigher) {
        mToolsUi->meta_button->setHidden(true);
        mToolsUi->open_keyboard_page_button->setHidden(true);
    }

    if (getConsoleAgents()->settings->android_cmdLineOptions()->fuchsia) {
        // These don't apply to Fuchsia
        mToolsUi->prev_layout_button->setHidden(true);
        mToolsUi->next_layout_button->setHidden(true);
        mToolsUi->back_button->setHidden(true);
        mToolsUi->home_button->setHidden(true);
        mToolsUi->overview_button->setHidden(true);
    }

    if (avdFlavor == AVD_XR) {
        mToolsUi->prev_layout_button->setHidden(true);
        mToolsUi->next_layout_button->setHidden(true);
    }

#ifndef Q_OS_MAC
    // Swap minimize and close buttons on non-apple OSes
    auto closeBtn = mToolsUi->winButtonsLayout->takeAt(0);
    mToolsUi->winButtonsLayout->insertItem(1, closeBtn);
#endif
    // How big is the toolbar naturally? This is just tall enough
    // to show all the buttons, so we should never shrink it
    // smaller than this.

    if (android_foldable_hinge_configured() ||
        android_foldable_folded_area_configured(0) ||
        android_foldable_rollable_configured()) {
        mFoldableSyncToAndroid.start();
    } else {
        mToolsUi->change_posture_button->setHidden(true);
    }
    updateFoldableButtonVisibility();
    updateXrButtonsVisibility();

    const bool isGlassesAVD = (avdFlavor == AVD_GLASSES);
    if (isGlassesAVD) {
        mToolsUi->zoom_button->setHidden(true);
        mToolsUi->prev_layout_button->setHidden(true);
        mToolsUi->next_layout_button->setHidden(true);
        mToolsUi->home_button->setHidden(true);
        mToolsUi->overview_button->setHidden(true);
        mToolsUi->glasses_button->setEnabled(true);
        if (getConsoleAgents()->settings->hw()->hw_touchpad0) {
            mTouchpadWindow->get();
        }
    } else {
        mToolsUi->glasses_button->setVisible(false);
    }

    bool showEnvironmentMenu = isGlassesAVD;
    if (!showEnvironmentMenu &&
        android::base::System::get()->getEnvironmentVariable(
                "ANDROID_EMU_ENABLE_ENVIRONMENT_MENU") == "1") {
        dinfo("ANDROID_EMU_ENABLE_ENVIRONMENT_MENU is set, enabling environmentMenu");
        showEnvironmentMenu = true;
    }

    if (showEnvironmentMenu) {
        QMenu* envMenu = createEnvironmentMenu();
        mToolsUi->environment_button->setMenu(envMenu);
        mToolsUi->environment_button->setEnabled(true);
    } else {
        mToolsUi->environment_button->setVisible(false);
    }

    connect(mPostureSelectionDialog, SIGNAL(newPostureRequested(int)), this,
            SLOT(onNewPostureRequested(int)));
    connect(mPostureSelectionDialog, SIGNAL(finished(int)), this,
            SLOT(onDismissPostureSelectionDialog()));
    connect(mResizableDialog,
            SIGNAL(newResizableRequested(PresetEmulatorSizeType)), this,
            SLOT(onNewResizableRequested(PresetEmulatorSizeType)));
    connect(mResizableDialog, SIGNAL(finished(int)), this,
            SLOT(onDismissResizableDialog()));
    if (android_is_xr_vst_headset_mode()) {
        connect(mXrEnvironmentModeDialog, SIGNAL(onXrEnvironmentModeRequested(int)),
                this, SLOT(onXrEnvironmentModeChanged(int)));
        connect(mXrEnvironmentModeDialog,
                SIGNAL(onXrPassthroughCoefficientRequested(float)), this,
                SLOT(onXrPassthroughCoefficientChanged(float)));
        connect(mXrEnvironmentModeDialog,
                SIGNAL(onXrDimmingValueRequested(float, bool)), this,
                SLOT(onXrDimmingValueChanged(float, bool)));
        connect(mXrEnvironmentModeDialog, SIGNAL(finished(int)),
                this, SLOT(onDismissXrEnvironmentModeDialog()));
        connect(mXrInputModeDialog, SIGNAL(onXrInputModeRequested(int)), this,
                SLOT(onXrInputModeChanged(int)));
        connect(mXrInputModeDialog, SIGNAL(finished(int)), this,
                SLOT(onDismissXrInputModeDialog()));

        connect(mLeftHandDialog, &LeftHandDialog::leftHandGestureSelected, this,
                &ToolWindow::onLeftHandGestureChanged);
        connect(mRightHandDialog, &RightHandDialog::rightHandGestureSelected,
                this, &ToolWindow::onRightHandGestureChanged);
    }

    if (!resizableEnabled()) {
        mToolsUi->resizable_button->setHidden(true);
    } else {
        resizableChangeIcon(getResizableActiveConfigId());
    }

    if (!getConsoleAgents()
                 ->settings->android_cmdLineOptions()
                 ->hands_in_space) {
        mToolsUi->left_hand_button->setHidden(true);
        mToolsUi->right_hand_button->setHidden(true);
    }

    if (leanToolbar()) {
        // Hide everything else; all_apps_button remains the home button for
        // AVDs that use AllAppsForHomeTray.
        const QPushButton* keep[] = {
                mToolsUi->close_button,    mToolsUi->minimize_button,
                mToolsUi->back_button,     mToolsUi->home_button,
                mToolsUi->all_apps_button,
        };
        for (QPushButton* button : findChildren<QPushButton*>()) {
            if (std::find(std::begin(keep), std::end(keep), button) ==
                std::end(keep)) {
                button->setHidden(true);
            }
        }
    }

    if (leanToolbar()) {
        // Let dockMainWindow() stretch the lean toolbar to the guest screen.
        // Rebuild the button order so navigation stays at the bottom while
        // the two window controls remain at the top.
        mToolsUi->mainLayout->setSizeConstraint(QLayout::SetNoConstraint);
        mToolsUi->controlsLayout->setSizeConstraint(QLayout::SetNoConstraint);
        mToolsUi->mainLayout->setAlignment(Qt::AlignHCenter);
        mToolsUi->controlsLayout->setAlignment(Qt::AlignHCenter);
        setMinimumSize(0, 0);
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        const int toolbarWidth = sizeHint().width();
        setMinimumWidth(toolbarWidth);
        setMaximumWidth(toolbarWidth);

        mToolsUi->winButtonsLayout->removeWidget(mToolsUi->close_button);
        mToolsUi->winButtonsLayout->removeWidget(mToolsUi->minimize_button);
        mToolsUi->winButtonsLayout->addWidget(mToolsUi->minimize_button);
        mToolsUi->winButtonsLayout->addWidget(mToolsUi->close_button);

        mToolsUi->controlsLayout->removeWidget(mToolsUi->back_button);
        mToolsUi->controlsLayout->removeWidget(mToolsUi->home_button);
        mToolsUi->controlsLayout->removeWidget(mToolsUi->all_apps_button);
        mToolsUi->controlsLayout->addStretch(1);
        mToolsUi->controlsLayout->addWidget(mToolsUi->back_button);
        mToolsUi->controlsLayout->addWidget(mToolsUi->home_button);
        mToolsUi->controlsLayout->addWidget(mToolsUi->all_apps_button);

        mToolsUi->minimize_button->setToolTip(QString::fromUtf8("最小化"));
        mToolsUi->close_button->setToolTip(QString::fromUtf8("关机"));
        mToolsUi->back_button->setToolTip(QString::fromUtf8("返回"));
        mToolsUi->home_button->setToolTip(QString::fromUtf8("主页"));
        mToolsUi->all_apps_button->setToolTip(QString::fromUtf8("主页"));
        if (inWindowNav()) {
            mToolsUi->close_button->hide();
            mToolsUi->minimize_button->hide();
        }
    }

    if (inWindowNav()) {
        createWindowNavColumn();
    }

    sToolWindow = this;
    skin_winsys_touch_qt_extended_virtual_sensors();

    remaskButtons();
    installEventFilter(this);

    mSleepTimer.setSingleShot(true);
    connect(&mSleepTimer, SIGNAL(timeout()), this, SLOT(onSleepTimerDone()));
    mUnfoldTimer.setSingleShot(true);
    connect(&mUnfoldTimer, SIGNAL(timeout()), this, SLOT(onUnfoldTimerDone()));
}

ToolWindow::~ToolWindow() {
    auto notifier = static_cast<android::base::EventNotificationSupport<bool>*>(
            getConsoleAgents()->vm->getRealAudioEventListener());
    mFoldableSyncToAndroid.enqueue({
            SEND_EXIT,
    });
    mFoldableSyncToAndroid.join();
}

void ToolWindow::startSleepTimer() {
    mSleepTimer.start(2000); // 2 second
}

void ToolWindow::startUnfoldTimer(PresetEmulatorSizeType newSize) {
    mDesiredNewSize = newSize;
    onNewPostureRequested(POSTURE_OPENED);
    mUnfoldTimer.start(2000);
}

void ToolWindow::onSleepTimerDone() {
    if (android_getShouldSkipDraw()) {
        android_setShouldSkipDraw(false);
        android_redrawOpenglesWindow();
    }
    if (isResizableTransitionInProgress()) {
        setResizableTransitionInProgress(false);
        updateFoldableButtonVisibility();
    }
    if (mSleepKeySent) {
        mEmulatorWindow->getAdbInterface()->
            enqueueCommand( {"shell", "input", "keyevent", "KEYCODE_WAKEUP"});
    }
}

void ToolWindow::onUnfoldTimerDone() {
    onNewResizableRequested(mDesiredNewSize);
}

void ToolWindow::updateFoldableButtonVisibility() {
    mToolsUi->change_posture_button->setEnabled(
            android_foldable_hinge_enabled());
    mExtendedWindow.ifExists([&] {
        mExtendedWindow.get()->getVirtualSensorsPage()->updateHingeSensorUI(); });
}

void ToolWindow::updateXrButtonsVisibility() {
    auto is_xr_mode = android_is_xr_vst_headset_mode();
    for (const auto &[xrButtonType, xrButtonList] :
            mXrButtonTypeToPushButtonsMap) {
        for (const auto& xrButtonRef : xrButtonList) {
            QPushButton& xrButton = xrButtonRef.get();
            if (!is_xr_mode) {
                xrButton.setHidden(true);
            }
            xrButton.setEnabled(is_xr_mode);
        }
    }
    updateXrNavigationButtonsChecked(mXrLastMouseKeyboardModeCommand);
}

#ifdef __APPLE__
void ToolWindow::setTouchpadWindowMacParent(WId wid) {
    if (!mTouchpadWindow.hasInstance())
        return;
    mTouchpadWindow.get()->showNormal();  // force creation of native window id
    WId tp_wid = mTouchpadWindow.get()->effectiveWinId();
    tp_wid = (WId)getNSWindow((void*)tp_wid);
    if (wid && tp_wid) {
        nsWindowAdopt((void*)wid, (void*)tp_wid);
    }
}
#endif

void ToolWindow::updateButtonUiCommand(QPushButton* button,
                                       const char* uiCommand) {
    button->setProperty("uiCommand", uiCommand);
    QtUICommand cmd;
    if (parseQtUICommand(QString(uiCommand), &cmd)) {
        QVector<QKeySequence>* shortcuts = mShortcutKeyStore.reverseLookup(cmd);
        if (shortcuts && shortcuts->length() > 0) {
            button->setToolTip(
                    getQtUICommandDescription(cmd) + " (" +
                    shortcuts->at(0).toString(QKeySequence::NativeText) + ")");
        }
    }
}

void ToolWindow::raise() {
    if (mVirtualSceneControlWindow.hasInstance() &&
        mVirtualSceneControlWindow.get()->isActive()) {
        mVirtualSceneControlWindow.get()->raise();
    }
    if (mTouchpadWindow.hasInstance()) {
        mTouchpadWindow.get()->raise();
    }
    if (mTopSwitched) {
        mExtendedWindow.ifExists([&] {
            mExtendedWindow.get()->raise();
            mExtendedWindow.get()->activateWindow();
        });
        mTopSwitched = false;
    }
}

void ToolWindow::allowExtWindowCreation() {
    mAllowExtWindow = true;
}

void ToolWindow::switchClipboardSharing(bool enabled) {
    if (sUiEmuAgent && sUiEmuAgent->clipboard) {
        sUiEmuAgent->clipboard->setEnabled(enabled);
    }
}

void ToolWindow::ensureVirtualSceneWindowCreated() {
    // Creates the virtual scene control window if it does not exist.
    mVirtualSceneControlWindow.get();
}

void ToolWindow::showVirtualSceneControls(bool show) {
    mVirtualSceneControlWindow.get()->setActiveForCamera(show);
}

void ToolWindow::onExtendedWindowCreated(ExtendedBaseWindow* extendedWindow) {
    if (auto userActionsCounter = mUserActionsCounter.lock()) {
        userActionsCounter->startCountingForExtendedWindow(extendedWindow);
    }

    setupSubwindow(extendedWindow);

    mVirtualSceneControlWindow.ifExists([&] {
        extendedWindow->connectVirtualSceneWindow(
                mVirtualSceneControlWindow.get().get());
    });
}

void ToolWindow::onVirtualSceneWindowCreated(
        VirtualSceneControlWindow* virtualSceneWindow) {
    if (auto userActionsCounter = mUserActionsCounter.lock()) {
        userActionsCounter->startCountingForVirtualSceneWindow(
                virtualSceneWindow);
    }

    setupSubwindow(virtualSceneWindow);

    mExtendedWindow.ifExists([&] {
        mExtendedWindow.get()->connectVirtualSceneWindow(virtualSceneWindow);
    });
}

void ToolWindow::onTouchpadWindowCreated(TouchpadWindow* touchpadWindow) {
    setupSubwindow(touchpadWindow);
    SettingsTheme theme = getSelectedTheme();
    touchpadWindow->setStyleSheet(Ui::stylesheetForTheme(theme));
}

void ToolWindow::setupSubwindow(QWidget* window) {
    if (auto recorderPtr = mUIEventRecorder.lock()) {
        recorderPtr->startRecording(window);
    }

    // If window is created before it's activated (for example, before the "..."
    // button is pressed for the extended window) it should be hid until it is
    // actually activated.
    window->hide();
}

void ToolWindow::hide() {
    QFrame::hide();
    mVirtualSceneControlWindow.ifExists(
            [&] { mVirtualSceneControlWindow.get()->hide(); });
    mTouchpadWindow.ifExists([&] { mTouchpadWindow.get()->hide(); });
    hideExtendedWindow();
}

void ToolWindow::closeEvent(QCloseEvent* ce) {
    ce->ignore();
    on_close_button_clicked();
}

void ToolWindow::mousePressEvent(QMouseEvent* event) {
    raiseMainWindow();
    QFrame::mousePressEvent(event);
}

void ToolWindow::hideEvent(QHideEvent*) {
    mIsExtendedWindowVisibleOnShow = mExtendedWindow.hasInstance() &&
                                     mExtendedWindow.get()->isVisible() &&
                                     mExtendedWindow.get()->isActiveWindow();
    mIsVirtualSceneWindowVisibleOnShow =
            mVirtualSceneControlWindow.hasInstance() &&
            mVirtualSceneControlWindow.get()->isVisible();
}

void ToolWindow::show() {
    setVisible(true);
    if (leanToolbar()) {
        setFixedWidth(sizeHint().width());
    } else {
        setFixedSize(size());
    }

    if (mIsVirtualSceneWindowVisibleOnShow) {
        mVirtualSceneControlWindow.get()->show();
    }

    if (mTouchpadWindow.hasInstance()) {
        mTouchpadWindow.get()->show();
    }

    if (mIsExtendedWindowVisibleOnShow) {
    mExtendedWindow.ifExists([&] { mExtendedWindow.get()->show(); });
    }
}

void ToolWindow::setVisible(bool visible) {
    if (inWindowNav()) {
        QFrame::setVisible(false);
        if (mWindowNavColumn) {
            mWindowNavColumn->setVisible(true);
            positionWindowNavColumn();
        }
        return;
    }
    QFrame::setVisible(visible);
}

void ToolWindow::createWindowNavColumn() {
    if (mWindowNavColumn) {
        return;
    }
    mWindowNavColumn = new QWidget(mEmulatorWindow->containerWindow());
    mWindowNavColumn->setObjectName(QStringLiteral("windowNavColumn"));
    mWindowNavColumn->setStyleSheet(QStringLiteral(
            "#windowNavColumn { background: #20212D; }"
            "#windowNavColumn QPushButton { background: #20212D; color: #E8E8EC; border: 0; }"
            "#windowNavColumn QPushButton:hover { background: #303142; }"
            "#windowNavColumn QPushButton:pressed { background: #3B3D52; }"
            "#windowNavColumn QPushButton:disabled { color: #777985; background: #20212D; }"));
    mWindowNavColumn->setFocusPolicy(Qt::NoFocus);
    auto* layout = new QVBoxLayout(mWindowNavColumn);
    layout->setContentsMargins(10, 0, 10, 0);
    layout->setSpacing(0);
    layout->addStretch(1);

    auto addButton = [&](QPushButton* source, auto pressed, auto released) {
        if (source->isHidden()) {
            return;
        }
        auto* button = new QPushButton(mWindowNavColumn);
        button->setObjectName(QStringLiteral("windowNavButton"));
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedSize(34, 34);
        button->setIcon(source->icon());
        button->setIconSize(source->iconSize());
        button->setProperty("themeIconName", source->property("themeIconName"));
        button->setProperty("ToolWindowButton", true);
        button->setProperty("iconSize", source->iconSize());
        button->setProperty("minimumSize", QSize(34, 34));
        button->setToolTip(source->toolTip());
        QObject::connect(button, &QPushButton::pressed, this, pressed);
        QObject::connect(button, &QPushButton::released, this, released);
        layout->addWidget(button, 0, Qt::AlignHCenter);
    };
    addButton(mToolsUi->back_button, &ToolWindow::on_back_button_pressed,
              &ToolWindow::on_back_button_released);
    QPushButton* homeSource = mToolsUi->home_button->isHidden()
                                      ? mToolsUi->all_apps_button
                                      : mToolsUi->home_button;
    if (homeSource == mToolsUi->all_apps_button) {
        addButton(homeSource, &ToolWindow::on_all_apps_button_pressed,
                  &ToolWindow::on_all_apps_button_released);
    } else {
        addButton(homeSource, &ToolWindow::on_home_button_pressed,
                  &ToolWindow::on_home_button_released);
    }
    mWindowNavColumn->show();
    positionWindowNavColumn();
}

void ToolWindow::positionWindowNavColumn() {
    if (!mWindowNavColumn || !inWindowNav()) {
        return;
    }
    auto* container = mEmulatorWindow->containerWindow();
    const QRect vr = container->viewport()->geometry();
    mWindowNavColumn->setGeometry(vr.right() + 1, vr.top(), 54, vr.height());
    mWindowNavColumn->raise();
}

void ToolWindow::syncWindowNavEnabled() {
    if (mWindowNavColumn) {
        mWindowNavColumn->setEnabled(isEnabled());
        if (safeShutdownInProgress()) {
            for (QPushButton* button : mWindowNavColumn->findChildren<QPushButton*>()) {
                button->setEnabled(false);
            }
        }
    }
}

void ToolWindow::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::EnabledChange) {
        syncWindowNavEnabled();
    }
}

bool ToolWindow::needExtendedWindow(QtUICommand cmd) const {
    switch (cmd) {
        case QtUICommand::HOME:
        case QtUICommand::BACK:
        case QtUICommand::OVERVIEW:
        case QtUICommand::META:
        case QtUICommand::POWER:
        case QtUICommand::TAKE_SCREENSHOT:
        case QtUICommand::VOLUME_DOWN:
        case QtUICommand::VOLUME_UP:
        case QtUICommand::GLASSES:
        case QtUICommand::TOGGLE_MICROPHONE:
        case QtUICommand::ALL_APPS:
            return false;
        default:
            return true;
    }
    return true;
}

void ToolWindow::ensureExtendedWindowExists() {
    if (mAllowExtWindow && !mIsExiting) {
        mExtendedWindow.get();
    }
}

bool ToolWindow::setUiTheme(SettingsTheme theme, bool persist) {
    if (theme < 0 || theme >= SETTINGS_THEME_NUM_ENTRIES) {
        // Out of range--ignore
        return false;
    }
    if (getSelectedTheme() != theme) {
        setSelectedTheme(theme, persist);
        ensureExtendedWindowExists();
        emit(themeChanged(theme));
    }
    return true;
}

void ToolWindow::showExtendedWindow(ExtendedWindowPane pane) {
    ensureExtendedWindowExists();
    if (pane == PANE_IDX_UNKNOWN) {
        on_more_button_clicked();
    } else {
        showOrRaiseExtendedWindow(pane);
    }
}

void ToolWindow::waitForExtendedWindowVisibility(bool visible) {
    if (!visible && !mExtendedWindow.hasInstance()) {
        // No extended window and we are wating to be hidden..
        // so no need to wait
        return;
    }
    ensureExtendedWindowExists();
    mExtendedWindow.get()->waitForVisibility(visible);
}

void ToolWindow::hideExtendedWindow() {
    mExtendedWindow.ifExists([&] { mExtendedWindow.get()->hide(); });
}

void ToolWindow::handleUICommand(QtUICommand cmd,
                                 bool down,
                                 std::string extra) {
    // Many UI commands require the extended window
    if (needExtendedWindow(cmd)) {
        ensureExtendedWindowExists();
    }

    switch (cmd) {
        case QtUICommand::SHOW_PANE_LOCATION:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_LOCATION);
            }
            break;
        case QtUICommand::SHOW_PANE_CELLULAR:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_CELLULAR);
            }
            break;
        case QtUICommand::SHOW_PANE_BATTERY:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_BATTERY);
            }
            break;
        case QtUICommand::SHOW_PANE_CAMERA:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_CAMERA);
            }
            break;
        case QtUICommand::SHOW_PANE_BUGREPORT:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_BUGREPORT);
            }
            break;
        case QtUICommand::SHOW_PANE_PHONE:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_TELEPHONE);
            }
            break;
        case QtUICommand::SHOW_PANE_MICROPHONE:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_MICROPHONE);
            }
            break;
        case QtUICommand::SHOW_PANE_VIRTSENSORS:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_VIRT_SENSORS);
            }
            break;
        case QtUICommand::SHOW_PANE_SNAPSHOT:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_SNAPSHOT);
            }
            break;
        case QtUICommand::SHOW_PANE_DPAD:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_DPAD);
            }
            break;
        case QtUICommand::SHOW_PANE_TV_REMOTE:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_TV_REMOTE);
            }
            break;
        case QtUICommand::SHOW_PANE_FINGER:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_FINGER);
            }
            break;
        case QtUICommand::SHOW_PANE_GPLAY:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_GOOGLE_PLAY);
            }
            break;
        case QtUICommand::SHOW_PANE_RECORD:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_RECORD);
            }
            break;
        case QtUICommand::SHOW_PANE_CAR:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_CAR);
            }
            break;
        case QtUICommand::SHOW_PANE_CAR_ROTARY:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_CAR_ROTARY);
            }
            break;
        case QtUICommand::SHOW_PANE_MULTIDISPLAY:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_MULTIDISPLAY);
            }
            break;
        case QtUICommand::SHOW_PANE_SETTINGS:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_SETTINGS);
            }
            break;
        case QtUICommand::SHOW_PANE_HELP:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_HELP);
            }
            break;
        case QtUICommand::TAKE_SCREENSHOT:
            if (down) {
                mEmulatorWindow->screenshot();
            }
            break;
        case QtUICommand::ENTER_ZOOM:
            if (down) {
                mEmulatorWindow->toggleZoomMode();
            }
            mToolsUi->zoom_button->setChecked(mEmulatorWindow->isInZoomMode());
            break;
        case QtUICommand::ZOOM_IN:
            if (down) {
                if (mEmulatorWindow->isInZoomMode()) {
                    mEmulatorWindow->zoomIn();
                } else {
                    mEmulatorWindow->scaleUp();
                }
            }
            break;
        case QtUICommand::ZOOM_OUT:
            if (down) {
                if (mEmulatorWindow->isInZoomMode()) {
                    mEmulatorWindow->zoomOut();
                } else {
                    mEmulatorWindow->scaleDown();
                }
            }
            break;
        case QtUICommand::PAN_UP:
            if (down) {
                mEmulatorWindow->panVertical(true);
            }
            break;
        case QtUICommand::PAN_DOWN:
            if (down) {
                mEmulatorWindow->panVertical(false);
            }
            break;
        case QtUICommand::PAN_LEFT:
            if (down) {
                mEmulatorWindow->panHorizontal(true);
            }
            break;
        case QtUICommand::PAN_RIGHT:
            if (down) {
                mEmulatorWindow->panHorizontal(false);
            }
            break;
        case QtUICommand::VOLUME_UP:
            forwardKeyToEmulator(LINUX_KEY_VOLUMEUP, down);
            break;
        case QtUICommand::VOLUME_DOWN:
            forwardKeyToEmulator(LINUX_KEY_VOLUMEDOWN, down);
            break;
        case QtUICommand::TOGGLE_MICROPHONE:
            setMicrophoneEnabled(!getMicrophoneEnabled());
            break;
        case QtUICommand::POWER:
            if (avdInfo_getAvdFlavor(
                        getConsoleAgents()->settings->avdInfo()) ==
                AVD_GLASSES) {
                forwardKeyToEmulator(ANDROID_KEY_STEM_2, down);
            } else {
                forwardKeyToEmulator(LINUX_KEY_POWER, down);
            }
            break;
        case QtUICommand::GLASSES:
            forwardKeyToEmulator(ANDROID_KEY_STEM_1, down);
            break;
        case QtUICommand::TABLET_MODE:
            // no-op, legacy usage for ChromeOS
            break;
        case QtUICommand::MENU:
            forwardKeyToEmulator(LINUX_KEY_SOFT1, down);
            break;
        case QtUICommand::HOME:
            forwardKeyToEmulator(LINUX_KEY_HOME, down);
            break;
        case QtUICommand::ALL_APPS:
            forwardKeyToEmulator(ANDROID_KEY_ALL_APPS, down);
            break;
        case QtUICommand::BACK:
            forwardKeyToEmulator(LINUX_KEY_BACK, down);
            break;
        case QtUICommand::OVERVIEW:
            if (avdInfo_isDesktopApi36OrHigher(getConsoleAgents()->settings->avdInfo())) {
                forwardKeyToEmulator(LINUX_KEY_SCALE, down);
            } else {
                forwardKeyToEmulator(ANDROID_KEY_APPSWITCH, down);
            }
            break;
        case QtUICommand::META:
            forwardKeyToEmulator(LINUX_KEY_LEFTMETA, down);
            break;
        case QtUICommand::WEAR_1:
            forwardKeyToEmulator(LINUX_KEY_HOME, down);
            break;
        case QtUICommand::WEAR_2:
            forwardKeyToEmulator(LINUX_KEY_POWER, down);
            break;
        case QtUICommand::PALM:
            forwardKeyToEmulator(LINUX_KEY_SLEEP, down);
            break;
        case QtUICommand::TILT:
            if (down) {
                float tilt = 1.0f;
                sUiEmuAgent->sensors->setPhysicalParameterTarget(
                        PHYSICAL_PARAMETER_WRIST_TILT, &tilt, 1,
                        PHYSICAL_INTERPOLATION_SMOOTH);
            }
            break;
        case QtUICommand::ROTATE_RIGHT:
        case QtUICommand::ROTATE_LEFT:
            if (down) {
                const int fromState = sUiEmuAgent->window->getRotation();
                const bool isRight = cmd == QtUICommand::ROTATE_RIGHT;
                SkinRotation nextRotation = SKIN_ROTATION_0;
                switch (fromState) {
                    case SKIN_ROTATION_0:
                        nextRotation =
                                isRight ? SKIN_ROTATION_90 : SKIN_ROTATION_270;
                        break;
                    case SKIN_ROTATION_90:
                        nextRotation =
                                isRight ? SKIN_ROTATION_180 : SKIN_ROTATION_0;
                        break;
                    case SKIN_ROTATION_180:
                        nextRotation =
                                isRight ? SKIN_ROTATION_270 : SKIN_ROTATION_90;
                        break;
                    case SKIN_ROTATION_270:
                        nextRotation =
                                isRight ? SKIN_ROTATION_0 : SKIN_ROTATION_180;
                        break;
                }

                sUiEmuAgent->sensors->setCoarseOrientation(
                        skin_rotation_to_coarse_orientation(nextRotation));
                skin_winsys_touch_qt_extended_virtual_sensors();
            }
            break;
        case QtUICommand::SHOW_PANE_SENSOR_REPLAY:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_SENSOR_REPLAY);
            }
            break;
        case QtUICommand::TOGGLE_TRACKBALL:
            if (down) {
                mEmulatorWindow->queueSkinEvent(createSkinEvent(kEventToggleTrackball));
            }
            break;
        case QtUICommand::CHANGE_FOLDABLE_POSTURE:
            if (down && mLastRequestedFoldablePosture != -1) {
                float posture =
                        static_cast<float>(mLastRequestedFoldablePosture);
                sUiEmuAgent->sensors->setPhysicalParameterTarget(
                        PHYSICAL_PARAMETER_POSTURE, &posture, 1,
                        PHYSICAL_INTERPOLATION_SMOOTH);
            }
            break;
        case QtUICommand::UPDATE_FOLDABLE_POSTURE_INDICATOR:
            if (down) {
                float posture = 0;
                float* out = &posture;
                sUiEmuAgent->sensors->getPhysicalParameter(
                        PHYSICAL_PARAMETER_POSTURE, &out, 1,
                        PARAMETER_VALUE_TYPE_CURRENT);
                applyFoldableQuirk((enum FoldablePostures)posture);
                switch ((enum FoldablePostures)posture) {
                    case POSTURE_OPENED:
                        ChangeIcon(mToolsUi->change_posture_button,
                                   "posture_open", "Change posture");
                        break;
                    case POSTURE_CLOSED:
                        ChangeIcon(mToolsUi->change_posture_button,
                                   "posture_closed", "Change posture");
                        break;
                    case POSTURE_HALF_OPENED:
                        ChangeIcon(mToolsUi->change_posture_button,
                                   "posture_half-open", "Change posture");
                        break;
                    case POSTURE_FLIPPED:
                        ChangeIcon(mToolsUi->change_posture_button,
                                   "posture_flipped", "Change posture");
                        break;
                    case POSTURE_TENT:
                        ChangeIcon(mToolsUi->change_posture_button,
                                   "posture_tent", "Change posture");
                        break;
                    default:;
                }
                const bool is_pixel_fold = android_foldable_is_pixel_fold();
                if (is_pixel_fold) {
                    mEmulatorWindow->resizeAndChangeAspectRatio(android_foldable_is_folded());
                } else {
                    if (android_foldable_is_folded()) {
                        int xOffset, yOffset, width, height;
                        if (android_foldable_get_folded_area(&xOffset, &yOffset,
                                                             &width, &height)) {
                            mEmulatorWindow->resizeAndChangeAspectRatio(true);
                            if (android_foldable_rollable_configured()) {
                                // rollable has up to 3 folded-areas, need
                                // guarntee the folded-area are updated in
                                // window manager before sending LID_CLOSE
                                mFoldableSyncToAndroid.enqueue({
                                        SEND_LID_OPEN,
                                });
                                mFoldableSyncToAndroid.enqueue(
                                        {SEND_AREA, xOffset, yOffset, width,
                                         height});
                                mFoldableSyncToAndroid.enqueue(
                                        {CONFIRM_AREA, xOffset, yOffset, width,
                                         height});
                            } else {
                                // hinge or legacy foldable has only one folded
                                // area. Once configured, no need to configure
                                // again. Unless explicitly required, e.g., in
                                // case of rebooting Android itselft only.
                                if (!mFoldableSyncToAndroidSuccess ||
                                    extra == "confirmFoldedArea") {
                                    mFoldableSyncToAndroid.enqueue(
                                            {SEND_AREA, xOffset, yOffset, width,
                                             height});
                                    mFoldableSyncToAndroid.enqueue(
                                            {CONFIRM_AREA, xOffset, yOffset,
                                             width, height});
                                } else {
                                    mFoldableSyncToAndroid.enqueue({
                                            SEND_LID_CLOSE,
                                    });
                                }
                            }
                        }
                    } else {
                        mEmulatorWindow->resizeAndChangeAspectRatio(false);
                        mFoldableSyncToAndroid.enqueue({
                                SEND_LID_OPEN,
                        });
                    }
                }
            }

            break;
        case QtUICommand::CHANGE_XR_INPUT_MODE:
            if (android_is_xr_vst_headset_mode()) {
                if (down) {
                    LOG(DEBUG) << "Sending XR Input Mode: "
                               << mLastInputModeRequested;
                    xr_service::sendInputMode(
                            static_cast<XrInputMode>(mLastInputModeRequested));
                    updateXrNavigationButtonsChecked(cmd);
                }
                setRelativeMouseMode(false);
            }
            break;
        case QtUICommand::CHANGE_XR_PASSTHROUGH_COEFFICIENT:
            if (android_is_xr_vst_headset_mode()) {
                xr_service::sendPassthroughCoefficient(
                        mLastPassthroughCoefficientRequested);
            }
            break;
        case QtUICommand::CHANGE_XR_ENVIRONMENT_MODE:
            if (android_is_xr_vst_headset_mode()) {
                xr_service::sendEnvironment(
                        static_cast<xr_emulator_proto::XrOptions_Environment>(
                                mLastEnvironmentRequested));
            }
            break;
        case QtUICommand::CHANGE_XR_DIMMING_VALUE:
            if (android_is_xr_vst_headset_mode()) {
                xr_service::sendDimmingValue(mLastDimmingValueRequested);
            }
            break;
        case QtUICommand::XR_SCREEN_RECENTER:
            if (android_is_xr_vst_headset_mode()) {
                if (down) {
                    LOG(DEBUG) << "Sending XR Screen Recenter";
                    xr_service::sendScreenRecenter();
                }
            }
            break;
        case QtUICommand::XR_VIEWPORT_CONTROL_MODE_PAN:
            setViewportControlMode(VIEWPORT_CONTROL_MODE_PAN, down, cmd);
            break;
        case QtUICommand::XR_VIEWPORT_CONTROL_MODE_DOLLY:
            setViewportControlMode(VIEWPORT_CONTROL_MODE_ZOOM, down, cmd);
            break;
        case QtUICommand::XR_VIEWPORT_CONTROL_MODE_ROTATE:
            setViewportControlMode(VIEWPORT_CONTROL_MODE_ROTATE, down, cmd);
            break;
        case QtUICommand::SHOW_MULTITOUCH:
        // Multitouch is handled in EmulatorQtWindow, and doesn't
        // really need an element in the QtUICommand enum. This
        // enum element exists solely for the purpose of displaying
        // it in the list of keyboard shortcuts in the Help page.
        default:;
    }
}

void ToolWindow::setViewportControlMode(XrViewportControlMode control,
                                        bool down,
                                        QtUICommand cmd) {
    if (android_is_xr_vst_headset_mode()) {
        if (down) {
            LOG(DEBUG) << "Sending XR Viewport Mode: " << control;
            xr_service::sendViewportControlMode(control);
            updateXrNavigationButtonsChecked(cmd);
        }
        setRelativeMouseMode(true);
    }
}

void ToolWindow::presetSizeAdvance(PresetEmulatorSizeType newSize) {
    if (!resizableEnabled()) {
        return;
    }
    if (getResizableActiveConfigId() == newSize) {
        return;
    }
    if (isResizableTransitionInProgress()) {
        return;
    }
    if (android_foldable_is_folded()) {
        startUnfoldTimer(newSize);
        return;
    }
    PresetEmulatorSizeInfo info;
    if (!getResizableConfig(newSize, &info)) {
        LOG(ERROR) << "Failed to get size information of resizable type "
                   << newSize;
        return;
    }

    // If folded, we need to unfold it first, otherwise it will mess up
    // non-foldable devices.
    if (getResizableActiveConfigId() == PRESET_SIZE_UNFOLDED &&
        mLastRequestedFoldablePosture == POSTURE_CLOSED) {
        // Directly call posture change function without going through emit,
        // because we want it to be processed before the resizable event.
        onNewPostureRequested(POSTURE_OPENED);
    }

    setResizableTransitionInProgress(true);
    android_setShouldSkipDraw(true);
    startSleepTimer();
    std::string updateMsg = "Updating device size\n";
    switch (info.type) {
        case PRESET_SIZE_PHONE:
            updateMsg += "Phone\n";
            break;
        case PRESET_SIZE_UNFOLDED:
            updateMsg += "Unfolded\n";
            break;
        case PRESET_SIZE_TABLET:
            updateMsg += "Tablet\n";
            break;
        default:;
    }
    updateMsg += std::to_string(info.width) + " x " +
                 std::to_string(info.height) + " dp\n" +
                 std::to_string(info.dpi) + " dpi";

    LOG(INFO) << "Resizable: change to new size: " << newSize;
    resizableChangeIcon(newSize);
    SkinEvent skin_event = createSkinEvent(kEventSetDisplayActiveConfig);
    skin_event.u.display_active_config = static_cast<int>(newSize);
    mEmulatorWindow->queueSkinEvent(std::move(skin_event));
    if (!resizableEnabled34()) {
        mEmulatorWindow->resizeAndChangeAspectRatio(0, 0, info.width,
                                                    info.height);
    }
    if (sUiEmuAgent && sUiEmuAgent->window) {
        sUiEmuAgent->window->showMessage(updateMsg.c_str(), WINDOW_MESSAGE_GENERIC,
                                         3000);
    }
    if (resizableEnabled34()) {
        mToolsUi->change_posture_button->setEnabled(newSize ==
                                                    PRESET_SIZE_UNFOLDED);
        if (mExtendedWindow.hasInstance()) {
            mExtendedWindow.get()
                    ->getVirtualSensorsPage()
                    ->updateHingeSensorUI();
        }
    } else {
        updateFoldableButtonVisibility();
    }
}

void ToolWindow::resizableChangeIcon(PresetEmulatorSizeType type) {
    switch (type) {
        case PRESET_SIZE_PHONE:
            ChangeIcon(mToolsUi->resizable_button, "display_mode_phone_expand",
                       "Display mode: Phone");
            break;
        case PRESET_SIZE_UNFOLDED:
            ChangeIcon(mToolsUi->resizable_button,
                       "display_mode_foldable_expand",
                       "Display mode: Foldable");
            break;
        case PRESET_SIZE_TABLET:
            ChangeIcon(mToolsUi->resizable_button, "display_mode_tablet_expand",
                       "Display mode: Tablet");
            break;
        default:
            LOG(ERROR) << "Invalid display mode " << type;
    }
}

// Fetch from the agent.
// MicrophonePage::loadSettings and the listener keep this up to date
bool ToolWindow::getMicrophoneEnabled() {
    return getConsoleAgents()->vm->isRealAudioAllowed();
}

void ToolWindow::setMicrophoneEnabled(bool enabled) {
    // Enable or disable host audio into the AVD
    getConsoleAgents()->vm->allowRealAudio(enabled);

    updateMicrophoneUI(enabled);
}

void ToolWindow::onExternalMicrophoneEnabledChanged(bool enabled) {
    updateMicrophoneUI(enabled);
}

void ToolWindow::updateMicrophoneUI(bool enabled) {
    if (enabled) {
        ChangeIcon(mToolsUi->microphone_button, "mic_enabled",
                   "Microphone: Enabled");
    } else {
        ChangeIcon(mToolsUi->microphone_button, "mic_disabled",
                   "Microphone: Disabled");
    }
}

// static
void ToolWindow::forwardGenericEventToEmulator(int type, int code, int value) {
    EmulatorQtWindow* emuQtWindow = EmulatorQtWindow::getInstance();
    if (emuQtWindow == nullptr) {
        VLOG(foldable) << "Error send Event, null emulator qt window";
        return;
    }

    SkinEvent skin_event = createSkinEvent(kEventGeneric);
    SkinEventGenericData& genericData = skin_event.u.generic_event;
    genericData.type = type;
    genericData.code = code;
    genericData.value = value;
    emuQtWindow->queueSkinEvent(std::move(skin_event));
}

void ToolWindow::forwardKeyToEmulator(uint32_t keycode, bool down) {
    SkinEvent skin_event = createSkinEvent(down ? kEventKeyDown : kEventKeyUp);
    skin_event.u.key.keycode = keycode;
    skin_event.u.key.mod = 0;
    mEmulatorWindow->queueSkinEvent(std::move(skin_event));
}

bool ToolWindow::handleQtKeyEvent(const QKeyEvent& event, QtKeyEventSource source) {
    // See if this key is handled by the virtual scene control window first.
    if (mVirtualSceneControlWindow.hasInstance() &&
        mVirtualSceneControlWindow.get()->isActive()) {
        if (mVirtualSceneControlWindow.get()->handleQtKeyEvent(event, source)) {
            return true;
        }
    }

    // If the touchpad is enabled, give it a chance to see the keys next.
    if (mTouchpadWindow.hasInstance()) {
        if (mTouchpadWindow.get()->handleQtKeyEvent(event, source)) {
            return true;
        }
    }

    // We don't care about the keypad modifier for anything, and it gets added
    // to the arrow keys of OSX by default, so remove it.
    QKeySequence event_key_sequence(event.key() |
                                    (event.modifiers() & ~Qt::KeypadModifier));
    bool down = event.type() == QEvent::KeyPress;
    bool h = mShortcutKeyStore.handle(event_key_sequence,
                                      [this, down](QtUICommand cmd) {
                                          if (down) {
                                              handleUICommand(cmd, true);
                                              handleUICommand(cmd, false);
                                          }
                                      });
    return h;
}

bool ToolWindow::isShortcutKey(const QKeyEvent& event) const {
    // Same key sequence as handleQtKeyEvent() looks up.
    QKeySequence event_key_sequence(event.key() |
                                    (event.modifiers() & ~Qt::KeypadModifier));
    return mShortcutKeyStore.handles(event_key_sequence);
}

void ToolWindow::reportMouseButtonDown() {
    mVirtualSceneControlWindow.ifExists(
            [&] { mVirtualSceneControlWindow.get()->reportMouseButtonDown(); });
}

bool ToolWindow::isExtendedWindowFocused() {
    if (mExtendedWindow.hasInstance()) {
        return mExtendedWindow.get()->isActiveWindow();
    }

    return false;
}

bool ToolWindow::isExtendedWindowVisible() {
    if (mExtendedWindow.hasInstance()) {
        return mExtendedWindow.get()->isVisible();
    }
    return false;
}

void ToolWindow::closeExtendedWindow() {
    mExtendedWindow.clear();
}

void ToolWindow::dockMainWindow() {
    if (inWindowNav()) {
        const int toolbarWidth = 54;
        if (auto* area = qobject_cast<EmulatorContainer*>(parentWidget())) {
            area->setLeanToolbarWidth(toolbarWidth);
            const QRect frame = area->frameGeometry();
            move(frame.right() + TOOL_GAP_FRAMED - width() + 1,
                 frame.top() + mEmulatorWindow->getTopTransparency());
            positionWindowNavColumn();
        }
    } else if (leanToolbar()) {
        const int toolbarHeight = std::max(1, parentWidget()->height() -
                                                   mEmulatorWindow->getTopTransparency() -
                                                   mEmulatorWindow->getBottomTransparency());
        if (height() != toolbarHeight) {
            setFixedHeight(toolbarHeight);
        }
    }

    // Align horizontally relative to the main window's frame.
    // Align vertically to its contents.
    // If we're frameless, adjust for a transparent border
    // around the skin.
    bool hasFrame;
    mEmulatorWindow->windowHasFrame(&hasFrame);
    int toolGap = hasFrame ? TOOL_GAP_FRAMED : TOOL_GAP_FRAMELESS;

    move(parentWidget()->frameGeometry().right() + toolGap -
                 mEmulatorWindow->getRightTransparency() + 1,
         parentWidget()->geometry().top() +
                 mEmulatorWindow->getTopTransparency());

    QPoint offset(0, 0);
    mTouchpadWindow.ifExists([&] {
        mTouchpadWindow->get()->dockMainWindow();
        offset += QPoint(0, mTouchpadWindow.get()->height());
    });
    mVirtualSceneControlWindow.ifExists(
            [&] { mVirtualSceneControlWindow.get()->dockMainWindow(offset); });
}

bool ToolWindow::isLeanToolbar() const {
    return leanToolbar();
}

int ToolWindow::leanToolbarWidth() const {
    return inWindowNav() ? 54 : (leanToolbar() ? std::max(1, sizeHint().width()) : 0);
}

bool ToolWindow::safeShutdownInProgress() const {
    return mEmulatorWindow && mEmulatorWindow->safeShutdownInProgress();
}

void ToolWindow::raiseMainWindow() {
    mEmulatorWindow->raise();
    mEmulatorWindow->activateWindow();
}

void ToolWindow::updateTheme(const QString& styleSheet) {
    mVirtualSceneControlWindow.ifExists(
            [&] { mVirtualSceneControlWindow.get()->updateTheme(styleSheet); });
    mTouchpadWindow.ifExists(
            [&] { mTouchpadWindow.get()->setStyleSheet(styleSheet); });
    setStyleSheet(styleSheet);
    if (mWindowNavColumn) {
        mWindowNavColumn->setStyleSheet(QStringLiteral(
                "#windowNavColumn { background: #20212D; }"
                "#windowNavColumn QPushButton { background: #20212D; color: #E8E8EC; border: 0; }"
                "#windowNavColumn QPushButton:hover { background: #303142; }"
                "#windowNavColumn QPushButton:pressed { background: #3B3D52; }"
                "#windowNavColumn QPushButton:disabled { color: #777985; background: #20212D; }"));
        remaskButtons();
    }
}

// static
void ToolWindow::earlyInitialization(const UiEmuAgent* agentPtr) {
    sUiEmuAgent = agentPtr;
    // This is crazy expensive!
    ExtendedWindow::setAgent(agentPtr);
    VirtualSceneControlWindow::setAgent(agentPtr);

    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (!avdPath) {
        // Can't find the setting!
        return;
    }

    QString avdSettingsFile =
            avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
    QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);

    SaveSnapshotOnExit saveOnExitChoice = static_cast<SaveSnapshotOnExit>(
            avdSpecificSettings
                    .value(Ui::Settings::SAVE_SNAPSHOT_ON_EXIT,
                           static_cast<int>(SaveSnapshotOnExit::Always))
                    .toInt());

    // Synchronize avdParams right here; avoid tight coupling with whether the
    // settings page is initialized.
    switch (saveOnExitChoice) {
        case SaveSnapshotOnExit::Always:
            getConsoleAgents()->settings->avdParams()->flags &=
                    !AVDINFO_NO_SNAPSHOT_SAVE_ON_EXIT;
            break;
        case SaveSnapshotOnExit::Ask:
            // If we can't ask, we'll treat ASK the same as ALWAYS.
            getConsoleAgents()->settings->avdParams()->flags &=
                    !AVDINFO_NO_SNAPSHOT_SAVE_ON_EXIT;
            break;
        case SaveSnapshotOnExit::Never:
            getConsoleAgents()->settings->avdParams()->flags |=
                    AVDINFO_NO_SNAPSHOT_SAVE_ON_EXIT;
            break;
        default:
            break;
    }
}

// static
void ToolWindow::onMainLoopStart() {
    if (sToolWindow) {
        sToolWindow->allowExtWindowCreation();
    }
}

void ToolWindow::setClipboardCallbacks(const UiEmuAgent* agPtr) {
    if (agPtr->clipboard) {
        connect(this, SIGNAL(guestClipboardChanged(QString)), this,
                SLOT(onGuestClipboardChanged(QString)), Qt::QueuedConnection);
        agPtr->clipboard->registerGuestClipboardCallback(
                [](void* context, const uint8_t* data, size_t size) {
                    LOG(DEBUG) << "Clipboard update, guest->host, value='"
                               << std::string_view(
                                          reinterpret_cast<const char*>(data),
                                          size)
                               << "'";

                    auto self = static_cast<ToolWindow*>(context);
                    emit self->guestClipboardChanged(
                            QString::fromUtf8((const char*)data, size));
                },
                this);
        connect(QApplication::clipboard(), SIGNAL(dataChanged()), this,
                SLOT(onHostClipboardChanged()));
    }

    mClipboardSupported = agPtr->clipboard != nullptr;
    emit haveClipboardSharingKnown(mClipboardSupported);
}

void ToolWindow::on_back_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::BACK, true);
}

void ToolWindow::on_back_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::BACK, false);
}

// If we need to ask about saving a snapshot,
// ask here, then set an avdParams flag to
// indicate the choice.
// If we don't need to ask, the avdParams flag
// should already be set.
// If the user cancels the pop-up, return
// 'false' to say we should NOT exit now.
bool ToolWindow::askWhetherToSaveSnapshot() {
    mAskedWhetherToSaveSnapshot = true;
    // Check the UI setting
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (!avdPath) {
        // Can't find the setting! Assume it's not ASK: just return;
        return true;
    }
    QString avdSettingsFile =
            avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
    QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);

    SaveSnapshotOnExit saveOnExitChoice = static_cast<SaveSnapshotOnExit>(
            avdSpecificSettings
                    .value(Ui::Settings::SAVE_SNAPSHOT_ON_EXIT,
                           static_cast<int>(SaveSnapshotOnExit::Always))
                    .toInt());

    if (getConsoleAgents()
                ->settings->android_cmdLineOptions()
                ->no_snapshot_save) {
        // The command line was used, so don't ask. That overrides the UI.
        return true;
    }

    if (saveOnExitChoice == SaveSnapshotOnExit::Never) {
        // The avdParams flag should already be set
        return true;
    }

    // If the setting is ALWAYS, we might want to ask anyway, such as when
    // previous saves were known to be slow, or the system has low RAM.
    bool savesWereSlow = androidSnapshot_areSavesSlow(
            android::snapshot::kDefaultBootSnapshot);

#if defined(__APPLE__) && defined(__aarch64__)
    // bug: 222536052
    bool hasLowRam = false;
#else
    bool hasLowRam = System::isUnderMemoryPressure();
#endif

    if (saveOnExitChoice == SaveSnapshotOnExit::Always &&
        (fc::isEnabled(fc::QuickbootFileBacked) ||
         (!savesWereSlow && !hasLowRam))) {
        return true;
    }

    // The setting is ASK or we decided to ask anyway.
    auto askMessageSlow = tr(
            "Do you want to save the current state for the next quick boot?\n\n"
            "Note: Recent saves seem to have been slow. Save can be skipped "
            "by selecting 'No'.");
    auto askMessageLowRam = tr(
            "Do you want to save the current state for the next quick boot?\n\n"
            "Note: Saving the snapshot may take longer because free RAM is "
            "low.");
    auto askMessageDefault = tr(
            "Do you want to save the current state for the next quick boot?");

    auto askMessageNonFileBacked =
            savesWereSlow ? askMessageSlow
                          : (hasLowRam ? askMessageLowRam : askMessageDefault);

    auto askMessage = fc::isEnabled(fc::QuickbootFileBacked)
                              ? tr("In the next emulator session, "
                                   "do you want to auto-save emulator state?")
                              : askMessageNonFileBacked;

    int64_t startTime = get_uptime_ms();
    QMessageBox msgBox(QMessageBox::Question, tr("Save quick-boot state"),
                       askMessage, (QMessageBox::Yes | QMessageBox::No),
                       mEmulatorWindow->containerWindow());
    // Add a Cancel button to enable the MessageBox's X.
    // Since embedded has already disconnected by this point, we always assume shutdown.
    if(!getConsoleAgents()
            ->settings->android_cmdLineOptions()
            ->qt_hide_window) {
        QPushButton* cancelButton = msgBox.addButton(QMessageBox::Cancel);
        // Hide the Cancel button so X is the only way to cancel.
        cancelButton->setHidden(true);
    }

    // ten seconds
    constexpr int timeout = 10000;
    QTimer::singleShot(timeout, (msgBox.button(QMessageBox::Yes)),
                       SLOT(animateClick()));
    int selection = msgBox.exec();

    int64_t endTime = get_uptime_ms();
    uint64_t dialogTime = endTime - startTime;

    MetricsReporter::get().report([dialogTime](proto::AndroidStudioEvent* event) {
        auto counts =
                event->mutable_emulator_details()->mutable_snapshot_ui_counts();
        counts->set_quickboot_ask_total_time_ms(
                dialogTime + counts->quickboot_ask_total_time_ms());
    });

    if (selection == QMessageBox::Cancel) {
        MetricsReporter::get().report([](proto::AndroidStudioEvent* event) {
            auto counts = event->mutable_emulator_details()
                                  ->mutable_snapshot_ui_counts();
            counts->set_quickboot_ask_canceled(
                    1 + counts->quickboot_ask_canceled());
        });
        mAskedWhetherToSaveSnapshot = false;
        return false;
    }

    if (selection == QMessageBox::Yes) {
        MetricsReporter::get().report([](proto::AndroidStudioEvent* event) {
            auto counts = event->mutable_emulator_details()
                                  ->mutable_snapshot_ui_counts();
            counts->set_quickboot_ask_yes(1 + counts->quickboot_ask_yes());
        });
        getConsoleAgents()->settings->avdParams()->flags &=
                !AVDINFO_NO_SNAPSHOT_SAVE_ON_EXIT;
    } else {
        MetricsReporter::get().report([](proto::AndroidStudioEvent* event) {
            auto counts = event->mutable_emulator_details()
                                  ->mutable_snapshot_ui_counts();
            counts->set_quickboot_ask_no(1 + counts->quickboot_ask_no());
        });
        getConsoleAgents()->settings->avdParams()->flags |=
                AVDINFO_NO_SNAPSHOT_SAVE_ON_EXIT;
    }
    return true;
}

bool ToolWindow::shouldClose() {
    // we already asked, just return the answer
    if (mAskedWhetherToSaveSnapshot) {
        return mShouldClose;
    }

    mShouldClose = askWhetherToSaveSnapshot();

    return mShouldClose;
}

void ToolWindow::on_close_button_clicked() {
    if (leanToolbar() && mEmulatorWindow->safeShutdownInProgress()) {
        QMessageBox box(QMessageBox::Question, QString::fromUtf8("正在关机"),
                        QString::fromUtf8("强制关闭？最近几秒写入的数据可能丢失"),
                        QMessageBox::Yes | QMessageBox::No,
                        mEmulatorWindow->containerWindow());
        box.button(QMessageBox::Yes)->setText(QString::fromUtf8("是"));
        box.button(QMessageBox::No)->setText(QString::fromUtf8("否"));
        const auto answer = box.exec();
        if (answer == QMessageBox::Yes) {
            mEmulatorWindow->forceCloseNow();
        }
        return;
    }

    mIsExiting = true;
    mCloseClicked = true;
    if (leanToolbar()) {
        if (!shouldClose()) {
            mAskedWhetherToSaveSnapshot = false;
            mIsExiting = false;
            mCloseClicked = false;
            return;
        }

        for (QPushButton* button : findChildren<QPushButton*>()) {
            button->setEnabled(false);
        }
        if (mWindowNavColumn) {
            for (QPushButton* button : mWindowNavColumn->findChildren<QPushButton*>()) {
                button->setEnabled(false);
            }
        }
        mEmulatorWindow->startSafeShutdown();
        return;
    }

    setEnabled(false);
    if (QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ShiftModifier)) {
        // The user shift-clicked on the X
        // This counts as us asking and having the user say "don't save"
        mExtendedWindow.ifExists([&] { mExtendedWindow.get()->sendMetricsOnShutDown(); });
        mAskedWhetherToSaveSnapshot = true;
        getConsoleAgents()->settings->avdParams()->flags |=
                AVDINFO_NO_SNAPSHOT_SAVE_ON_EXIT;
        mEmulatorWindow->requestClose();

        return;
    }

    if(shouldClose()) {
        mExtendedWindow.ifExists([&] { mExtendedWindow.get()->sendMetricsOnShutDown(); });
        mEmulatorWindow->requestClose();
    } else {
        mAskedWhetherToSaveSnapshot = false;
        setEnabled(true);
        mIsExiting = false;
        mCloseClicked = false;
    }
}

void ToolWindow::on_home_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::HOME, true);
}

void ToolWindow::on_home_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::HOME, false);
}

void ToolWindow::on_all_apps_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::ALL_APPS, true);
}

void ToolWindow::on_all_apps_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::ALL_APPS, false);
}

void ToolWindow::on_minimize_button_clicked() {
    // Using hide for MacOS to avoid extra minimized windows showing
    // up on the toolbar, as well as avoiding positioning issues on
    // unminimizing
#if defined (__linux__) || defined(__APPLE__)
    this->hide();
#else
    this->showMinimized();
#endif
    mEmulatorWindow->showMinimized();
    if (mTouchpadWindow.hasInstance()) {
#if defined (__linux__) || defined(__APPLE__)
        mTouchpadWindow.get()->hide();
#else
        mTouchpadWindow.get()->showMinimized();
#endif
    }
}

void ToolWindow::on_power_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::POWER, true);
}

void ToolWindow::on_power_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::POWER, false);
}

void ToolWindow::on_tablet_mode_button_clicked() {
    static bool tablet_mode;
    mEmulatorWindow->activateWindow();
    tablet_mode = !tablet_mode;
    ChangeIcon(mToolsUi->tablet_mode_button,
               tablet_mode ? "laptop_mode" : "tablet_mode",
               tablet_mode ? "Switch to laptop mode" : "Switch to tablet mode");
    handleUICommand(QtUICommand::TABLET_MODE, tablet_mode);
}

void ToolWindow::on_change_posture_button_clicked() {
    mPostureSelectionDialog->show();
    // Align pop-up posture selection dialog to the right of posture button.
    QRect geoTool = this->geometry();
    mPostureSelectionDialog->move(
            geoTool.right(),
            geoTool.top() + mToolsUi->change_posture_button->geometry().top());
}

void ToolWindow::onDismissPostureSelectionDialog() {
    mToolsUi->change_posture_button->setChecked(false);
}

void ToolWindow::onDismissResizableDialog() {
    mToolsUi->resizable_button->setChecked(false);
}

void ToolWindow::on_xr_environment_mode_button_clicked() {
    mXrEnvironmentModeDialog->setDimmingValue(mLastDimmingValueRequested);
    mXrEnvironmentModeDialog->show();
    // Align pop-up posture selection dialog to the right of posture button
    QRect geoTool = this->geometry();
    mXrEnvironmentModeDialog->move(
            geoTool.right(),
            geoTool.top() +
                    mToolsUi->xr_environment_mode_button->geometry().top());
}

void ToolWindow::on_xr_input_mode_button_clicked() {
    if (fc::isEnabled(fc::XrHandAndEyePointers)) {
        mXrInputModeDialog->show();
        // Align pop-up input selection dialog to the right of input button.
        QRect geoTool = this->geometry();
        mXrInputModeDialog->move(
                geoTool.right(),
                geoTool.top() + mToolsUi->xr_input_mode_button->geometry().top());
    } else {
        // Neither flag is set, so only "Mouse" mode is available.
        // Don't show the dialog, just set the mode.
        onXrInputModeChanged(XR_INPUT_MODE_MOUSE_KEYBOARD);
    }
}

void ToolWindow::on_xr_screen_recenter_button_clicked() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::XR_SCREEN_RECENTER, true);
}

void ToolWindow::onXrDimmingValueChanged(float value, bool fromGuest) {
    mLastDimmingValueRequested = value;
    if (!fromGuest) {
        handleUICommand(QtUICommand::CHANGE_XR_DIMMING_VALUE, true);
    }
}

void ToolWindow::onDismissXrEnvironmentModeDialog() {
    mToolsUi->xr_environment_mode_button->setChecked(false);
}

void ToolWindow::onDismissXrInputModeDialog() {
    updateXrNavigationButtonsChecked(mXrLastMouseKeyboardModeCommand);
}

void ToolWindow::on_xr_viewport_pan_button_clicked() {
    mEmulatorWindow->activateWindow();
    if (mXrLastMouseKeyboardModeCommand == QtUICommand::XR_VIEWPORT_CONTROL_MODE_PAN) {
        onXrInputModeChanged(mLastInputModeRequested);
    } else {
        handleUICommand(QtUICommand::XR_VIEWPORT_CONTROL_MODE_PAN, true);
    }
}

void ToolWindow::on_xr_viewport_dolly_button_clicked() {
    mEmulatorWindow->activateWindow();
    if (mXrLastMouseKeyboardModeCommand == QtUICommand::XR_VIEWPORT_CONTROL_MODE_DOLLY) {
        onXrInputModeChanged(mLastInputModeRequested);
    } else {
        handleUICommand(QtUICommand::XR_VIEWPORT_CONTROL_MODE_DOLLY, true);
    }
}

void ToolWindow::on_xr_viewport_rotate_button_clicked() {
    mEmulatorWindow->activateWindow();
    if (mXrLastMouseKeyboardModeCommand == QtUICommand::XR_VIEWPORT_CONTROL_MODE_ROTATE) {
        onXrInputModeChanged(mLastInputModeRequested);
    } else {
        handleUICommand(QtUICommand::XR_VIEWPORT_CONTROL_MODE_ROTATE, true);
    }
}

static QString formatEnvironmentName(const QString& fileName) {
    QString name = fileName;
    name.replace('-', ' ');
    name.replace('_', ' ');

    QStringList words = name.split(' ', Qt::SkipEmptyParts);
    for (int i = 0; i < words.size(); ++i) {
        if (!words[i].isEmpty()) {
            words[i][0] = words[i][0].toUpper();
            for (int j = 1; j < words[i].length(); ++j) {
                words[i][j] = words[i][j].toLower();
            }
        }
    }
    return words.join(' ');
}

QMenu* ToolWindow::createEnvironmentMenu() {
    QMenu* envMenu = new QMenu(this);
    std::string sdkRoot = android::ConfigDirs::getSdkRootDirectory();
    QDir envDir(QString::fromStdString(sdkRoot) + "/environments");

    envDir.setNameFilters(QStringList() << "*.jpg");
    envDir.setFilter(QDir::Files | QDir::NoSymLinks);

    QFileInfoList list = envDir.entryInfoList();

    if (!list.isEmpty()) {
        for (const QFileInfo& fileInfo : list) {
            // Reserve files ending in 360 for 360 image mode
            if (fileInfo.fileName().endsWith("360.jpg")) {
                continue;
            }
            QString displayName =
                    formatEnvironmentName(fileInfo.completeBaseName());
            QString filePath = fileInfo.absoluteFilePath();
            QAction* action = envMenu->addAction(displayName);
            connect(action, &QAction::triggered, this, [this, filePath]() {
                std::string command =
                        "scene.mode = imagefile:" + filePath.toStdString();
                getConsoleAgents()->virtual_scene->reloadEnvironment(
                        command.c_str());
            });
        }
    } else {
        envMenu->addAction(tr("Default Environment"), this,
                           SLOT(defaultEnvironmentSelected()));
    }
    envMenu->addAction(tr("None"), this, SLOT(noneSelected()));
    envMenu->addSeparator();
    envMenu->addAction(tr("Custom Image..."), this,
                       SLOT(customImageSelected()));
    envMenu->addAction(tr("Custom Video..."), this,
                       SLOT(customVideoSelected()));
    envMenu->addAction(tr("Custom 360 Image..."), this,
                       SLOT(custom360ImageSelected()));

    const std::string video360FeatureVar = System::get()->envGet("ANDROID_EMU_ENABLE_VIDEO360");
    const bool video360Enabled = (video360FeatureVar == "1");
    if (video360Enabled) {
        envMenu->addAction(tr("Custom 360 Video"), this,
                           SLOT(custom360VideoSelected()));
    }

    // Check if the streetview feature is enabled:
    const std::string streetViewFeatureVar = System::get()->envGet("ANDROID_EMU_ENABLE_STREETVIEW");
    const bool streetViewEnabled = (streetViewFeatureVar == "1");
    if (streetViewEnabled) {
        envMenu->addAction(tr("Street View"), this,
                           SLOT(streetViewSelected()));
    }

    QMenu* webcamMenu = new QMenu(tr("Webcams"), this);
    const QAndroidVirtualSceneAgent* agent = getConsoleAgents()->virtual_scene;
    bool enableWebcamMenu = agent && agent->enumerateWebcams;

    // Allow users to disable webcam menu, this flag can be removed once the
    // feature is stable enough for all users.
    if (enableWebcamMenu &&
        android::base::System::get()->getEnvironmentVariable(
                "ANDROID_EMU_ENABLE_WEBCAM_MENU") == "0") {
        dinfo("ANDROID_EMU_ENABLE_WEBCAM_MENU is set to 0, disabling webcam menu");
        enableWebcamMenu = false;
    }

    if (enableWebcamMenu) {
        struct Context {
            QMenu* menu;
            ToolWindow* window;
        } context = {webcamMenu, this};

        agent->enumerateWebcams(&context, [](void* opaque,
                                             const char* userFacingName,
                                             const char* label,
                                             const char* id) {
            auto* ctx = static_cast<Context*>(opaque);
            ToolWindow* window = ctx->window;
            QAction* action = ctx->menu->addAction(
                    QString::fromUtf8(userFacingName ? userFacingName : label));
            std::string id_str = id ? id : "";
            window->connect(action, &QAction::triggered, [window, id_str]() {
                window->webcamSelected(id_str);
            });
        });
    }
    if (!webcamMenu->isEmpty()) {
        envMenu->addMenu(webcamMenu);
    } else {
        delete webcamMenu;
    }

    return envMenu;
}

void ToolWindow::defaultEnvironmentSelected() {
    getConsoleAgents()->virtual_scene->reloadEnvironment(
            "scene.mode = imagefile");
}

void ToolWindow::noneSelected() {
    getConsoleAgents()->virtual_scene->reloadEnvironment(
            "scene.mode = color:#000000");
}

void ToolWindow::customImageSelected() {
    QString fileName = QFileDialog::getOpenFileName(
            this, tr("Select Image"), "", tr("Images (*.png *.jpg *.jpeg)"));
    if (!fileName.isEmpty()) {
        std::string command =
                "scene.mode = imagefile:" + fileName.toStdString();
        getConsoleAgents()->virtual_scene->reloadEnvironment(command.c_str());
    }
}

void ToolWindow::customVideoSelected() {
    QString fileName = QFileDialog::getOpenFileName(this, tr("Select Video"),
                                                    "", tr("Videos (*.mp4)"));
    if (!fileName.isEmpty()) {
        std::string command =
                "scene.mode = videofile:" + fileName.toStdString();
        getConsoleAgents()->virtual_scene->reloadEnvironment(command.c_str());
    }
}

void ToolWindow::custom360ImageSelected() {
    QString fileName =
            QFileDialog::getOpenFileName(this, tr("Select 360 Image"), "",
                                         tr("Images (*.png *.jpg *.jpeg)"));
    if (!fileName.isEmpty()) {
        std::string command = "scene.mode = image360:" + fileName.toStdString();
        getConsoleAgents()->virtual_scene->reloadEnvironment(command.c_str());
    }
}

void ToolWindow::custom360VideoSelected() {
    QString fileName = QFileDialog::getOpenFileName(
            this, tr("Select 360 Video"), "", tr("Videos (*.mp4)"));
    if (!fileName.isEmpty()) {
        std::string command = "scene.mode = video360:" + fileName.toStdString();
        getConsoleAgents()->virtual_scene->reloadEnvironment(command.c_str());
    }
}

void ToolWindow::streetViewSelected() {
    std::string command = "scene.mode = streetview";
    getConsoleAgents()->virtual_scene->reloadEnvironment(command.c_str());
}

void ToolWindow::webcamSelected(const std::string& id) {
    std::string command = "scene.mode = webcam:" + id;
    getConsoleAgents()->virtual_scene->reloadEnvironment(command.c_str());
}

// Set the current selected input mode button or viewport control mode button
// checked (activated). The parameter `currentMode` is the current selected
// command.
void ToolWindow::updateXrNavigationButtonsChecked(QtUICommand currentMode) {
    if (!android_is_xr_vst_headset_mode()) {
        return;
    }
    for (const auto &[buttonType, buttonList] : mXrButtonTypeToPushButtonsMap) {
        // Iterate through all the buttons of type `input_control` or
        // `viewport_control` and find the button whose uiCommand property
        // matches with the parameter `currentMode` command string.
        if (buttonType == "input_control" || buttonType == "viewport_control") {
            for (const auto& buttonRef : buttonList) {
                QPushButton& xrButton = buttonRef.get();
                const QVariant uiCommand = xrButton.property("uiCommand");
                if (uiCommand.isValid()) {
                    QtUICommand buttonCommand;
                    // Parse the `uiCommand` string to retrieve the QtUICommand
                    // object.
                    if (parseQtUICommand(uiCommand.toString(), &buttonCommand)) {
                        if (buttonCommand == currentMode) {
                            mXrLastMouseKeyboardModeCommand = currentMode;
                            // Check the QPushButton if uiCommand matches with
                            // currently selection.
                            xrButton.setChecked(true);
                            continue;
                        }
                    } else {
                        LOG(WARNING) << "UiCommand for XrButton not found";
                    }
                } else {
                    LOG(WARNING) << "Expected UiCommand for XR buttons";
                }
                // For all the buttons which do not find a match with selected
                // command, uncheck the QPushButton.
                xrButton.setChecked(false);
            }
        }
    }
}

void ToolWindow::on_resizable_button_clicked() {
    mResizableDialog->show();
    // Align pop-up resizableDialog to the right of resizable button
    QRect geoTool = this->geometry();
    mResizableDialog->move(
            geoTool.right(),
            geoTool.top() + mToolsUi->resizable_button->geometry().top());
}

void ToolWindow::onNewResizableRequested(PresetEmulatorSizeType newSize) {
    mEmulatorWindow->activateWindow();
    presetSizeAdvance(newSize);
}

void ToolWindow::on_volume_up_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::VOLUME_UP, true);
}
void ToolWindow::on_volume_up_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::VOLUME_UP, false);
}
void ToolWindow::on_volume_down_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::VOLUME_DOWN, true);
}
void ToolWindow::on_volume_down_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::VOLUME_DOWN, false);
}

void ToolWindow::on_microphone_button_clicked() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::TOGGLE_MICROPHONE, true);
}

void ToolWindow::on_glasses_button_pressed() {
    mEmulatorWindow->raise();

    handleUICommand(QtUICommand::GLASSES, true);
}

void ToolWindow::on_glasses_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::GLASSES, false);
}

void ToolWindow::on_overview_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::OVERVIEW, true);
}

void ToolWindow::on_overview_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::OVERVIEW, false);
}

void ToolWindow::on_meta_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::META, true);
}

void ToolWindow::on_meta_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::META, false);
}

void ToolWindow::on_open_keyboard_page_button_clicked() {
    showOrRaiseExtendedWindow(PANE_IDX_KEYBOARD);
}

void ToolWindow::on_wear_button_1_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::WEAR_1, true);
}

void ToolWindow::on_wear_button_1_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::WEAR_1, false);
}

void ToolWindow::on_wear_button_2_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::WEAR_2, true);
}

void ToolWindow::on_wear_button_2_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::WEAR_2, false);
}

void ToolWindow::on_palm_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::PALM, true);
}

void ToolWindow::on_palm_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::PALM, false);
}

void ToolWindow::on_tilt_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::TILT, true);
}

void ToolWindow::on_tilt_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::TILT, false);
}

void ToolWindow::on_prev_layout_button_clicked() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::ROTATE_LEFT);
}

void ToolWindow::on_next_layout_button_clicked() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::ROTATE_RIGHT);
}

void ToolWindow::on_scrShot_button_clicked() {
    handleUICommand(QtUICommand::TAKE_SCREENSHOT, true);
}
void ToolWindow::on_zoom_button_clicked() {
    handleUICommand(QtUICommand::ENTER_ZOOM, true);
}

void ToolWindow::onGuestClipboardChanged(QString text) {
    QSignalBlocker blockSignals(QApplication::clipboard());
    QApplication::clipboard()->setText(text);
}

void ToolWindow::onHostClipboardChanged() {
    QByteArray bytes = QApplication::clipboard()->text().toUtf8();
    sUiEmuAgent->clipboard->setGuestClipboardContents(
            (const uint8_t*)bytes.data(), bytes.size());
}

static bool isPaneEnabled(ExtendedWindowPane pane) {
    // Snapshot pane is disabled in embedded emulator.
    if (pane == PANE_IDX_SNAPSHOT &&
        getConsoleAgents()
                ->settings->android_cmdLineOptions()
                ->qt_hide_window) {
        return false;
    }

    return true;
}

void ToolWindow::showOrRaiseExtendedWindow(ExtendedWindowPane pane) {
    if (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
        AVD_ANDROID_AUTO) {
        if (pane == PANE_IDX_DPAD || pane == PANE_IDX_BATTERY ||
            pane == PANE_IDX_FINGER || pane == PANE_IDX_CAMERA ||
            pane == PANE_IDX_MULTIDISPLAY) {
            return;
        }
    }
    if (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
                AVD_TV ||
        avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
                AVD_WEAR) {
        if (pane == PANE_IDX_MULTIDISPLAY) {
            return;
        }
    }
    if (!androidHwConfig_hasVirtualSceneOrEnvironmentCamera(
                getConsoleAgents()->settings->hw()) &&
        pane == PANE_IDX_CAMERA) {
        return;
    }

    // Set to default help pane.
    if (!isPaneEnabled(pane)) {
        pane = PANE_IDX_HELP;
    }
    // Show the tabbed pane
    mExtendedWindow.get()->showPane(pane);
    mExtendedWindow.get()->raise();
    mExtendedWindow.get()->activateWindow();
}

void ToolWindow::on_more_button_clicked() {
    if (mAllowExtWindow && !mIsExiting) {
        mExtendedWindow.get()->show();
        mExtendedWindow.get()->raise();
        mExtendedWindow.get()->activateWindow();
    }
}

void ToolWindow::paintEvent(QPaintEvent*) {
    QPainter p;
    QPen pen(Qt::SolidLine);
    pen.setColor(Qt::black);
    pen.setWidth(1);
    p.begin(this);
    p.setPen(pen);

    double dpr = 1.0;
    auto newScreen = window()->windowHandle()
                             ? window()->windowHandle()->screen()
                             : nullptr;
    if (!newScreen) {
        newScreen = qGuiApp->primaryScreen();
    }
    dpr = newScreen->devicePixelRatio();

    if (dpr > 1.0) {
        // Normally you'd draw the border with a (0, 0, w-1, h-1) rectangle.
        // However, there's some weirdness going on with high-density displays
        // that makes a single-pixel "slack" appear at the left and bottom
        // of the border. This basically adds 1 to compensate for it.
        p.drawRect(contentsRect());
    } else {
        p.drawRect(QRect(0, 0, width() - 1, height() - 1));
    }
    p.end();
    rebuildBackingStoreIfStale();
}

// Qt resizes a window's backing store only when the window's logical size
// changes. When just the device pixel ratio changes under this fixed-size
// window (e.g. the emulator started while the monitor was off and Windows
// reported 96 DPI, then the monitor came back at 150%), the store keeps its old
// device size and only its top-left part of the toolbar gets painted. Resize
// the store once the image it paints into no longer matches the window; at
// most once per ratio, so a mismatch the resize cannot fix never loops.
void ToolWindow::rebuildBackingStoreIfStale() {
    QBackingStore* store = backingStore();
    QPaintDevice* device = store ? store->paintDevice() : nullptr;
    const qreal dpr = devicePixelRatio();
    if (!device || dpr == mBackingStoreRebuiltDpr) {
        return;
    }
    if (std::abs(device->width() - width() * dpr) <= 2 &&
        std::abs(device->height() - height() * dpr) <= 2) {
        return;
    }
    mBackingStoreRebuiltDpr = dpr;
    QTimer::singleShot(0, this, [this] {
        if (QBackingStore* store = backingStore()) {
            // Go through another size first in case a resize to the same
            // logical size is skipped.
            store->resize(size() + QSize(1, 1));
            store->resize(size());
        }
        update();
    });
}

void ToolWindow::notifySwitchOnTop() {
#ifdef _WIN32
    mTopSwitched = true;
#endif
}

void ToolWindow::touchExtendedWindow() {
    mExtendedWindow.get();
}

void ToolWindow::enableCloseButton() {
    mToolsUi->close_button->setVisible(true);
}

void ToolWindow::hideRotationButton(bool hide) {
    if (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
                AVD_TV ||
        avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
                AVD_ANDROID_AUTO ||
        avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
                AVD_DESKTOP) {
        // already hide, do not bother its settings
        return;
    } else if (leanToolbar()) {
        // rotation buttons stay hidden in the lean toolbar
        return;
    } else {
        mToolsUi->prev_layout_button->setHidden(hide);
        mToolsUi->next_layout_button->setHidden(hide);
    }
}

void ToolWindow::applyFoldableQuirk(int newPosture) {
    const bool is_pixel_fold = android_foldable_is_pixel_fold();
    if (is_pixel_fold) {
        if (newPosture > 1 && mLastRequestedFoldablePosture == 1 ||
                newPosture == 1 && mLastRequestedFoldablePosture > 1) {
            auto hw = (getConsoleAgents()->settings->avdInfo());
            auto apiLevel = avdInfo_getApiLevel(
                    getConsoleAgents()->settings->avdInfo());
            if (avdInfo_isVanillaIceCreamPreview(hw) || apiLevel == 35) {
                mEmulatorWindow->getAdbInterface()->
                    enqueueCommand( {"shell", "input", "keyevent", "KEYCODE_SLEEP"});
                mSleepKeySent = true;
                startSleepTimer();
            }
        }
    }
    mLastRequestedFoldablePosture = newPosture;
}

void ToolWindow::onNewPostureRequested(int newPosture) {
    mEmulatorWindow->activateWindow();
    applyFoldableQuirk(newPosture);
    handleUICommand(QtUICommand::CHANGE_FOLDABLE_POSTURE, true);
}

void ToolWindow::onXrEnvironmentModeChanged(int mode) {
    mLastEnvironmentRequested = static_cast<int>(
            android::emulation::control::XrEnvironmentModeToGuestEnvironment(
                    static_cast<XrEnvironmentMode>(mode)));

    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::CHANGE_XR_ENVIRONMENT_MODE, true);
}

void ToolWindow::onXrPassthroughCoefficientChanged(float value) {
    mLastPassthroughCoefficientRequested = value;

    if (!mXrEnvironmentModeDialog->hasDimmingLevels()) {
        mEmulatorWindow->activateWindow();
    }
    handleUICommand(QtUICommand::CHANGE_XR_PASSTHROUGH_COEFFICIENT, true);
}

void ToolWindow::onXrInputModeChanged(int mode) {
    if (mode == /*XR_INPUT_MODE_MOUSE_UNKNOWN*/ 0) {
        LOG(WARNING) << "Unknown XR input mode requested: " << mode;
        return;
    }

    mEmulatorWindow->activateWindow();
    mLastInputModeRequested = mode;
    handleUICommand(QtUICommand::CHANGE_XR_INPUT_MODE, true);
    switch (mLastInputModeRequested) {
        case /*XR_INPUT_MODE_MOUSE_KEYBOARD*/ 1:
            ChangeIcon(mToolsUi->xr_input_mode_button,
                       "xr_input_mouse_keyboard_mode",
                       "Input: Mouse + Keyboard");
            break;
        case /*XR_INPUT_MODE_HAND_RAYCAST*/ 2:
            ChangeIcon(mToolsUi->xr_input_mode_button, "xr_input_hand_raycast",
                       "Input: Hand Raycast + Selection");
            break;
        case /*XR_INPUT_MODE_EYE_TRACKING*/ 3:
            ChangeIcon(mToolsUi->xr_input_mode_button, "xr_input_eye_tacking",
                       "Input: Eye Tracking + Selection");
            break;
        default:
            break;
    }
}

WorkerProcessingResult ToolWindow::foldableSyncToAndroidItemFunction(
        const FoldableSyncToAndroidItem& item) {
    switch (item.op) {
        case SEND_AREA: {
            EmulatorQtWindow* emuQtWindow = EmulatorQtWindow::getInstance();
            if (emuQtWindow == nullptr) {
                break;
            }
            char foldedArea[64];
            sprintf(foldedArea, "folded-area %d,%d,%d,%d", item.x, item.y,
                    item.x + item.width, item.y + item.height);
            std::string sendArea(foldedArea);
            emuQtWindow->getAdbInterface()->enqueueCommand(
                    {"shell", "wm", foldedArea},
                    [sendArea](const OptionalAdbCommandResult& result) {
                        if (result && result->exit_code == 0) {
                            VLOG(foldable) << "foldable-page: 'send "
                                           << sendArea << "' command succeeded";
                        }
                    });
            break;
        }
        case CONFIRM_AREA: {
            EmulatorQtWindow* emuQtWindow = EmulatorQtWindow::getInstance();
            if (emuQtWindow) {
                mFoldableSyncToAndroidSuccess = false;
                mFoldableSyncToAndroidTimeout = false;
                int64_t timeOut = System::get()->getUnixTimeUs() +
                                  30000 * 1000;  // 30 second time out
                // Keep on querying folded area through adb,
                // until query returns the expected values
                while (!mFoldableSyncToAndroidSuccess &&
                       !mFoldableSyncToAndroidTimeout) {
                    android::base::AutoLock lock(mLock);
                    emuQtWindow->getAdbInterface()->enqueueCommand(
                            {"shell", "wm", "folded-area"},
                            [this, item,
                             timeOut](const OptionalAdbCommandResult& result) {
                                android::base::AutoLock lock(this->mLock);
                                if (System::get()->getUnixTimeUs() > timeOut) {
                                    LOG(ERROR) << "time out (30 sec) waiting "
                                                  "for window manager "
                                                  "configuring folded area";
                                    this->mFoldableSyncToAndroidTimeout = true;
                                    this->mCv.signalAndUnlock(&lock);
                                    return;
                                }
                                if (!result || !result->output) {
                                    VLOG(foldable) << "Invalid output for wm "
                                                      "adb-area, query again";
                                    this->mFoldableSyncToAndroidSuccess = false;
                                    this->mCv.signalAndUnlock(&lock);
                                    return;
                                }
                                std::string line;
                                std::string expectedAdbOutput =
                                        "Folded area: " +
                                        std::to_string(item.x) + "," +
                                        std::to_string(item.y) + "," +
                                        std::to_string(item.x + item.width) +
                                        "," +
                                        std::to_string(item.y + item.height);
                                while (getline(*(result->output), line)) {
                                    if (line.compare(expectedAdbOutput) == 0) {
                                        this->mFoldableSyncToAndroidSuccess =
                                                true;
                                        break;
                                    }
                                }
                                this->mCv.signalAndUnlock(&lock);
                            });
                    // block thread until adb query for folded-area return
                    if (!mCv.timedWait(&mLock, timeOut)) {
                        // Something unexpected along adb communication thread,
                        // call back function not been called after timeOut
                        LOG(ERROR) << "adb no response for more than 30 "
                                      "seconds, time out";
                        mFoldableSyncToAndroidTimeout = true;
                    }
                    // send LID_CLOSE when expected folded-area is configured in
                    // window manager
                    if (mFoldableSyncToAndroidSuccess) {
                        VLOG(foldable) << "confirm folded area configured by "
                                          "window manager, send LID close";
                        forwardGenericEventToEmulator(EV_SW, SW_LID, true);
                        forwardGenericEventToEmulator(EV_SYN, 0, 0);
                    }
                }
            }
            break;
        }
        case SEND_LID_CLOSE:
            forwardGenericEventToEmulator(EV_SW, SW_LID, true);
            forwardGenericEventToEmulator(EV_SYN, 0, 0);
            VLOG(foldable) << "send LID close";
            break;
        case SEND_LID_OPEN:
            forwardGenericEventToEmulator(EV_SW, SW_LID, false);
            forwardGenericEventToEmulator(EV_SYN, 0, 0);
            VLOG(foldable) << "send LID open";
            break;
        case SEND_EXIT:
            return WorkerProcessingResult::Stop;
    }
    return WorkerProcessingResult::Continue;
}

void ToolWindow::remaskButtons() {
    QList<QPushButton*> buttons = findChildren<QPushButton*>();
    if (mWindowNavColumn) {
        buttons += mWindowNavColumn->findChildren<QPushButton*>();
    }
    for (QPushButton* button : buttons) {
        const QString icon_name = button->property("themeIconName").toString();
        if (!icon_name.isNull()) {
            double dpr = 1.0;
#ifndef Q_OS_MAC
            QScreen* scr = this->screen();
            if (scr) {
                dpr = scr->logicalDotsPerInch() / SizeTweaker::BaselineDpi;
            }
#endif
            const QSize icon_size = button->property("iconSize").toSize();
            const QSize min_size = button->property("minimumSize").toSize();
            button->setMinimumSize(min_size * dpr);
            button->setIconSize(icon_size * dpr);
        }
    }
}

bool ToolWindow::eventFilter(QObject* o, QEvent* event) {
    if (event->type() == QEvent::ScreenChangeInternal) {
        // When moved across screens, masks on buttons need to
        // be adjusted according to screen density.
        remaskButtons();
        if (leanToolbar()) {
            dockMainWindow();
        }
    } else if (event->type() == QEvent::WindowStateChange) {
        // It seems on mac, repainting after getting QEvent::ScreenChangeInternal is too early.
        // From experiments, asking for repaint after WindowStateChange will resize the tool-window
        // to the correct size. Furthermore, if the window moves too fast to a different display,
        // QEvent::WindowStateChange may not trigger. So a workaround is to also trigger a repaint
        // when the user releases the mouse from moving the window.
        repaint();
    }
    return false;
}

void ToolWindow::updateLeftHandButton(const QString& gesture) {
    auto data = LEFT_HAND_GESTURE_DATA.find(gesture);
    if (data == LEFT_HAND_GESTURE_DATA.end()) {
        LOG(ERROR) << "Unsupported gesture from UI: " << gesture.toStdString();
        return;
    }

    ChangeIcon(mToolsUi->left_hand_button, data->second.button_name,
               data->second.button_tip);
}

void ToolWindow::updateRightHandButton(const QString& gesture) {
    auto data = RIGHT_HAND_GESTURE_DATA.find(gesture);
    if (data == RIGHT_HAND_GESTURE_DATA.end()) {
        LOG(ERROR) << "Unsupported gesture from UI: " << gesture.toStdString();
        return;
    }

    ChangeIcon(mToolsUi->right_hand_button, data->second.button_name,
               data->second.button_tip);
}

void ToolWindow::notifyGuestOnLeftHandGesture(const QString& gesture) {
    auto data = LEFT_HAND_GESTURE_DATA.find(gesture);
    if (data == LEFT_HAND_GESTURE_DATA.end()) {
        return;
    }
    xr_service::sendHandGesture(data->second.protoId);
    mCurrentLeftHandGesture = gesture;
}

void ToolWindow::notifyGuestOnRightHandGesture(const QString& gesture) {
    auto data = RIGHT_HAND_GESTURE_DATA.find(gesture);
    if (data == RIGHT_HAND_GESTURE_DATA.end()) {
        return;
    }
    xr_service::sendHandGesture(data->second.protoId);
    mCurrentRightHandGesture = gesture;
}

void ToolWindow::on_left_hand_button_clicked() {
    mLeftHandDialog->show();
    QRect geoTool = this->geometry();
    mLeftHandDialog->move(
            geoTool.right(),
            geoTool.top() + mToolsUi->left_hand_button->geometry().top());

    // make only this button checked
    updateXrNavigationButtonsChecked(QtUICommand::XR_LEFT_HAND);

    notifyGuestOnLeftHandGesture(mCurrentLeftHandGesture);
}

void ToolWindow::on_right_hand_button_clicked() {
    mRightHandDialog->show();
    QRect geoTool = this->geometry();
    mRightHandDialog->move(
            geoTool.right(),
            geoTool.top() + mToolsUi->right_hand_button->geometry().top());

    // make only this button checked
    updateXrNavigationButtonsChecked(QtUICommand::XR_RIGHT_HAND);

    notifyGuestOnRightHandGesture(mCurrentRightHandGesture);
}

void ToolWindow::onLeftHandGestureChanged(const QString& gesture) {
    setRelativeMouseMode(false);

    updateLeftHandButton(gesture);

    notifyGuestOnLeftHandGesture(gesture);
}

void ToolWindow::onRightHandGestureChanged(const QString& gesture) {
    setRelativeMouseMode(false);

    updateRightHandButton(gesture);

    notifyGuestOnRightHandGesture(gesture);
}

void ToolWindow::injectAgents(const UiEmuAgent* agentPtr) {
    const auto win = EmulatorQtWindow::getInstance();
    win->runOnUiThread(
            [this, agentPtr] {
                int elapsedUs = 0;
                android::base::measure(elapsedUs, [this, agentPtr] {
                    mExtendedWindow.get()->injectAgents(agentPtr);
                });
                LOG(INFO) << "Created extended window in "
                          << absl::FromChrono(
                                     std::chrono::microseconds(elapsedUs));
            });
}
