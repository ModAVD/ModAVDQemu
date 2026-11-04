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

#pragma once

#include <stdint.h>
#include <memory>
#include <string>

#include <QCloseEvent>
#include <QFrame>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QRect>
#include <QString>
#include <QWidget>
#include <QPushButton>
#include <QTimer>

#include "aemu/base/Compiler.h"
#include "aemu/base/EventNotificationSupport.h"
#include "aemu/base/containers/CircularBuffer.h"
#include "aemu/base/memory/OnDemand.h"
#include "aemu/base/synchronization/ConditionVariable.h"
#include "aemu/base/synchronization/Lock.h"
#include "aemu/base/threads/WorkerThread.h"
#include "android/emulation/resizable_display_config.h"
#include "android/skin/qt/extended-window-base.h"
#include "android/skin/qt/hand-constants.h"
#include "android/skin/qt/left-hand-dialog.h"
#include "android/skin/qt/qt-ui-commands.h"
#include "android/skin/qt/resizable-dialog.h"
#include "android/skin/qt/right-hand-dialog.h"
#include "android/skin/qt/shortcut-key-store.h"
#include "android/skin/qt/touchpad-window.h"
#include "android/skin/qt/ui-event-recorder.h"
#include "android/skin/qt/user-actions-counter.h"
#include "android/skin/qt/virtualscene-control-window.h"
#include "android/ui-emu-agent.h"
#include "android/xr-defines.h"
#include "host-common/qt_ui_defs.h"

class LeanTitleBar;
class PostureSelectionDialog;
class XrEnvironmentModeDialog;
class XrInputModeDialog;

namespace Ui {
class ToolControls;
}

class ToolWindow : public QFrame {
    Q_OBJECT
    using UIEventRecorderPtr =
            std::weak_ptr<UIEventRecorder<android::base::CircularBuffer>>;
    using UserActionsCounterPtr =
            std::weak_ptr<android::qt::UserActionsCounter>;

    template <typename T>
    class WindowHolder {
        DISALLOW_COPY_AND_ASSIGN(WindowHolder);

    public:
        using OnCreatedCallback = void (ToolWindow::*)(T*);

        WindowHolder(ToolWindow* tw, OnCreatedCallback onCreated);
        ~WindowHolder();
        T* operator->() const { return mWindow; }
        T* get() const { return mWindow; }

    protected:
        WindowHolder() = default;
        T* mWindow;
    };

    class ExtendedWindowHolder : public WindowHolder<ExtendedBaseWindow> {
    public:
        ExtendedWindowHolder(ToolWindow* tw, OnCreatedCallback onCreated);
    };

public:
    ToolWindow(EmulatorQtWindow* emulatorWindow,
               QWidget* parent,
               UIEventRecorderPtr event_recorder,
               UserActionsCounterPtr user_actions_counter);
    ~ToolWindow();

    void allowExtWindowCreation();
    void hide();
    void show();
    void setVisible(bool visible) override;
    void dockMainWindow();
    void raiseMainWindow();
    void updateTheme(const QString& styleSheet);

    void setClipboardCallbacks(const UiEmuAgent* agPtr);

    static void earlyInitialization(const UiEmuAgent* agentPtr);
    static void onMainLoopStart();
    static const UiEmuAgent* getUiEmuAgent() { return sUiEmuAgent; }

    const ShortcutKeyStore<QtUICommand>* getShortcutKeyStore() {
        return &mShortcutKeyStore;
    }

    bool handleQtKeyEvent(const QKeyEvent& event, QtKeyEventSource source);
    // Whether the shortcut table has |event|; looks only, runs nothing.
    bool isShortcutKey(const QKeyEvent& event) const;
    void reportMouseButtonDown();

    bool isExtendedWindowFocused();
    bool isExtendedWindowVisible();
    void closeExtendedWindow();
    void enableCloseButton();
    void updateFoldableButtonVisibility();
    void updateXrButtonsVisibility();
#ifdef __APPLE__
    void setTouchpadWindowMacParent(WId wid);
#endif

    // Observed only on Windows:
    // Whenever we set the window flags for the EmulatorContainer,
    // we receive a focusInEvent, which raises the tool window. We
    // need to raise the extended window after the tool window is
    // raised, which is what this function is for.
    void notifySwitchOnTop();

    // The designers want a gap between the main emulator
    // window and the tool bar. This is how big that gap is.
    static const int TOOL_GAP_FRAMED = 0;
    static const int TOOL_GAP_FRAMELESS = 4;

    bool shouldClose();

    bool closeButtonClicked() const { return mCloseClicked; }
    bool safeShutdownInProgress() const;
    // The lean toolbar's close: Android powers off and the emulator quits
    // once it has. The windows go away at once unless |hideWindows| is false,
    // as when Windows ends the session.
    void beginLeanSafeShutdown(bool hideWindows);

    bool isLeanToolbar() const;
    // Whether the window draws its own title bar with the navigation and
    // window buttons (ModAVD#58): the lean toolbar on Windows, with a window.
    static bool inWindowNav();
    // What the window adds around the guest screen, in logical pixels: the
    // title bar's height with inWindowNav(), nothing otherwise.
    static QSize windowChrome();
    LeanTitleBar* leanTitleBar() const { return mLeanTitleBar; }

    bool clipboardSharingSupported() const { return mClipboardSupported; }
    void forwardKeyToEmulator(uint32_t keycode, bool down);
    void touchExtendedWindow();
    // Handle a full key press (down + up) in a single call.
    void handleUICommand(QtUICommand cmd, std::string extra = "") {
        handleUICommand(cmd, true, extra);
        handleUICommand(cmd, false, extra);
    }
    void hideRotationButton(bool hide);
    bool setUiTheme(SettingsTheme theme, bool persist);
    void showExtendedWindow(ExtendedWindowPane pane);
    void hideExtendedWindow();

    void waitForExtendedWindowVisibility(bool visible);
    void presetSizeAdvance(PresetEmulatorSizeType newSize);
    void injectAgents(const UiEmuAgent* agentPtr);

signals:
    void guestClipboardChanged(QString text);
    void haveClipboardSharingKnown(bool have);
    void themeChanged(SettingsTheme theme);
    // Signal for callback to trigger the state to change;
    void _externalMicrophoneEnabledChanged(bool enabled);

private:
    bool eventFilter(QObject* o, QEvent* event) override;
    void remaskButtons();
    static void forwardGenericEventToEmulator(int type, int code, int value);
    void handleUICommand(QtUICommand cmd, bool down, std::string extra = "");
    void ensureExtendedWindowExists();
    bool needExtendedWindow(QtUICommand cmd) const;

    void stopExtendedWindowCreation();

    void onExtendedWindowCreated(ExtendedBaseWindow* extendedWindow);
    void onVirtualSceneWindowCreated(
            VirtualSceneControlWindow* virtualSceneWindow);
    void onTouchpadWindowCreated(TouchpadWindow* touchpadWindow);
    void setupSubwindow(QWidget* window);

    bool isExiting() const { return mIsExiting; }
    bool askWhetherToSaveSnapshot();
    void resizableChangeIcon(PresetEmulatorSizeType type);

    bool getMicrophoneEnabled();
    void setMicrophoneEnabled(bool enabled);
    void updateMicrophoneUI(bool enabled);

    void showOrRaiseExtendedWindow(ExtendedWindowPane pane);
    void updateButtonUiCommand(QPushButton* button, const char* uiCommand);
    // Check the input or viewport control button which matches the
    // `currentMode` command, otherwise uncheck the button.
    void updateXrNavigationButtonsChecked(QtUICommand currentMode);
    QMenu* createEnvironmentMenu();

    // Notify guest (Android) about the hand gesture selection for left hand.
    void notifyGuestOnLeftHandGesture(const QString& gesture);
    // Notify guest (Android) about the hand gesture selection for right hand.
    void notifyGuestOnRightHandGesture(const QString& gesture);
    // Update UI (e.g. icon, tooltip) of the button.
    void updateLeftHandButton(const QString& gesture);
    // Update UI (e.g. icon, tooltip) of the button.
    void updateRightHandButton(const QString& gesture);

    virtual void closeEvent(QCloseEvent* ce) override;
    void changeEvent(QEvent* event) override;
    virtual void mousePressEvent(QMouseEvent* event) override;
    virtual void paintEvent(QPaintEvent*) override;
    virtual void hideEvent(QHideEvent* event) override;
    void rebuildBackingStoreIfStale();
    void createLeanTitleBar();
    void positionLeanTitleBar();
    void syncLeanTitleBarEnabled();

    EmulatorQtWindow* mEmulatorWindow;
    android::base::MemberOnDemandT<ExtendedWindowHolder,
                                   ToolWindow*,
                                   void (ToolWindow::*)(ExtendedBaseWindow*)>
            mExtendedWindow;
    android::base::MemberOnDemandT<WindowHolder<VirtualSceneControlWindow>,
                                   ToolWindow*,
                                   void (ToolWindow::*)(
                                           VirtualSceneControlWindow*)>
            mVirtualSceneControlWindow;
    android::base::MemberOnDemandT<WindowHolder<TouchpadWindow>,
                                   ToolWindow*,
                                   void (ToolWindow::*)(TouchpadWindow*)>
            mTouchpadWindow;
    std::unique_ptr<Ui::ToolControls> mToolsUi;
    LeanTitleBar* mLeanTitleBar = nullptr;
    bool mStartedAdbStopProcess = false;
    ShortcutKeyStore<QtUICommand> mShortcutKeyStore;
    bool mIsExtendedWindowVisibleOnShow = false;
    bool mIsVirtualSceneWindowVisibleOnShow = false;
    QString mDetectedAdbPath;
    UIEventRecorderPtr mUIEventRecorder;
    UserActionsCounterPtr mUserActionsCounter;
    bool mTopSwitched = false;
    qreal mBackingStoreRebuiltDpr = 0;

    bool mIsExiting = false;
    bool mAskedWhetherToSaveSnapshot = false;
    bool mShouldClose = false;
    bool mAllowExtWindow = false;
    bool mClipboardSupported = false;

    int mLastRequestedFoldablePosture = -1;
    int mLastEnvironmentRequested = 0;
    float mLastDimmingValueRequested = 0.0f;
    float mLastPassthroughCoefficientRequested = 1.0f;
    int mLastInputModeRequested = /*XR_INPUT_MODE_MOUSE_KEYBOARD*/ 1;
    QtUICommand mXrLastMouseKeyboardModeCommand = QtUICommand::CHANGE_XR_INPUT_MODE;
    std::vector<std::reference_wrapper<QPushButton>> mXrMouseKeyboardModeButtons;

    QString mCurrentLeftHandGesture = Ui::GESTURE_NAME_PINCH;
    QString mCurrentRightHandGesture = Ui::GESTURE_NAME_PINCH;

    static const UiEmuAgent* sUiEmuAgent;

    PostureSelectionDialog* mPostureSelectionDialog;
    enum FoldableSyncToAndroidOp {
        SEND_AREA = 0,
        CONFIRM_AREA,
        SEND_LID_CLOSE,
        SEND_LID_OPEN,
        SEND_EXIT,
    };
    struct FoldableSyncToAndroidItem {
        enum FoldableSyncToAndroidOp op;
        int x, y, width, height;
    };
    android::base::WorkerThread<FoldableSyncToAndroidItem>
            mFoldableSyncToAndroid;
    android::base::WorkerProcessingResult foldableSyncToAndroidItemFunction(
            const FoldableSyncToAndroidItem& item);
    android::base::Lock mLock;
    android::base::ConditionVariable mCv;
    bool mFoldableSyncToAndroidSuccess;
    bool mFoldableSyncToAndroidTimeout;
    ResizableDialog* mResizableDialog;
    XrEnvironmentModeDialog* mXrEnvironmentModeDialog;
    XrInputModeDialog* mXrInputModeDialog;
    LeftHandDialog* mLeftHandDialog;
    RightHandDialog* mRightHandDialog;
    // A map of all XR specific pushbuttons in the main panel, which is used
    // to identify XR specific buttons and also group them together based on
    // specific type.
    std::unordered_map<std::string,
                       std::vector<std::reference_wrapper<QPushButton>>>
            mXrButtonTypeToPushButtonsMap;

    bool mCloseClicked = false;

    QTimer mSleepTimer;
    bool mSleepKeySent = false;

    void startSleepTimer();

    void applyFoldableQuirk(int newPosture);

    QTimer mUnfoldTimer;
    PresetEmulatorSizeType mDesiredNewSize {PRESET_SIZE_PHONE};
    void startUnfoldTimer(PresetEmulatorSizeType newSize);

    std::unique_ptr<android::base::RaiiEventListener<
            android::base::EventNotificationSupport<bool>,
            bool>>
            mRealAudioEventListener;

public slots:
    void raise();
    void switchClipboardSharing(bool enabled);
    void showVirtualSceneControls(bool show);
    void ensureVirtualSceneWindowCreated();
    void on_close_button_clicked();

private slots:
    // Signal handler for host microphone input toggle changes
    void onExternalMicrophoneEnabledChanged(bool enabled);
    void on_back_button_pressed();
    void on_back_button_released();
    void on_home_button_pressed();
    void on_home_button_released();
    void on_all_apps_button_pressed();
    void on_all_apps_button_released();
    void on_minimize_button_clicked();
    void on_more_button_clicked();
    void on_power_button_pressed();
    void on_power_button_released();
    void on_overview_button_pressed();
    void on_overview_button_released();
    void on_meta_button_pressed();
    void on_meta_button_released();
    void on_open_keyboard_page_button_clicked();
    void on_wear_button_1_pressed();
    void on_wear_button_1_released();
    void on_wear_button_2_pressed();
    void on_wear_button_2_released();
    void on_palm_button_pressed();
    void on_palm_button_released();
    void on_tilt_button_pressed();
    void on_tilt_button_released();
    void on_prev_layout_button_clicked();
    void on_next_layout_button_clicked();
    void on_scrShot_button_clicked();
    void on_volume_down_button_pressed();
    void on_volume_down_button_released();
    void on_volume_up_button_pressed();
    void on_volume_up_button_released();
    void on_microphone_button_clicked();
    void on_zoom_button_clicked();
    void on_tablet_mode_button_clicked();
    void on_change_posture_button_clicked();
    void onNewPostureRequested(int newPosture);
    void onDismissPostureSelectionDialog();
    void onNewResizableRequested(PresetEmulatorSizeType newSize);
    void onDismissResizableDialog();
    void on_resizable_button_clicked();
    void onGuestClipboardChanged(QString text);
    void onHostClipboardChanged();
    void on_xr_environment_mode_button_clicked();
    void onXrEnvironmentModeChanged(int mode);
    void onXrPassthroughCoefficientChanged(float value);
    void onXrDimmingValueChanged(float value, bool fromGuest);
    void onDismissXrEnvironmentModeDialog();
    void on_xr_input_mode_button_clicked();
    void onXrInputModeChanged(int mode);
    void onDismissXrInputModeDialog();
    void on_left_hand_button_clicked();
    void on_right_hand_button_clicked();
    void onLeftHandGestureChanged(const QString& gesture);
    void onRightHandGestureChanged(const QString& gesture);
    void on_xr_screen_recenter_button_clicked();
    void on_xr_viewport_pan_button_clicked();
    void on_xr_viewport_dolly_button_clicked();
    void on_xr_viewport_rotate_button_clicked();
    void defaultEnvironmentSelected();
    void noneSelected();
    void customImageSelected();
    void customVideoSelected();
    void custom360ImageSelected();
    void custom360VideoSelected();
    void streetViewSelected();
    void webcamSelected(const std::string& id);
    void on_glasses_button_pressed();
    void on_glasses_button_released();
    void setViewportControlMode(XrViewportControlMode control,
                                bool down,
                                QtUICommand cmd);

    void onSleepTimerDone();
    void onUnfoldTimerDone();
};
