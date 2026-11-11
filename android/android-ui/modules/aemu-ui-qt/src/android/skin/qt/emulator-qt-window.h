/* Copyright (C) 2015-2017 The Android Open Source Project
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

#include "aemu/base/containers/CircularBuffer.h"
#include "aemu/base/memory/OnDemand.h"
#include "aemu/base/synchronization/Lock.h"
#include "android/console.h"
#include "android/emulation/control/ApkInstaller.h"
#include "android/emulation/control/FilePusher.h"
#include "android/emulation/control/ScreenCapturer.h"
#include "android/metrics/PeriodicReporter.h"
#include "android/settings-agent.h"
#include "android/skin/event.h"
#include "android/skin/image.h"
// #include "android/skin/qt/car-cluster-window.h"
#include "android/skin/qt/emulator-container.h"
#include "android/skin/qt/emulator-overlay.h"
#include "android/skin/qt/error-dialog.h"
// #include "android/skin/qt/extended-pages/car-cluster-connector/car-cluster-connector.h"
// #include "android/skin/qt/multi-display-widget.h"
#include "android/skin/qt/tool-window.h"
#include "android/skin/qt/ui-event-recorder.h"
#include "android/skin/qt/user-actions-counter.h"
#include "android/skin/winsys.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QInputMethodEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressDialog>
#include <QTabletEvent>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Ui {
class EmulatorWindow;
}

typedef struct SkinSurface SkinSurface;
class SkinSurfaceBitmap;
class SwipeGesture;

class CarClusterWindow;
class CarClusterConnector;
class MultiDisplayWidget;
class SharedMemoryRenderer;
#include "android/skin/qt/SharedStreamEmulator.h"


using RunOnUiThreadFunc = std::function<void()>;
Q_DECLARE_METATYPE(QPainter::CompositionMode);
Q_DECLARE_METATYPE(SkinGenericFunction);
Q_DECLARE_METATYPE(SkinRotation);
Q_DECLARE_METATYPE(Ui::OverlayMessageType);

class MainLoopThread : public QThread {
    Q_OBJECT

public:
    MainLoopThread(StartFunction f, int argc, char** argv)
        : start_function(f), argc(argc), argv(argv) {}
    void run() Q_DECL_OVERRIDE {
        if (start_function)
            start_function(argc, argv);
    }

private:
    StartFunction start_function;
    int argc;
    char** argv;
};

class CustomInitProgressDialog : public QProgressDialog {
    Q_OBJECT

public:
    using InitFunc = std::function<void(QProgressDialog*)>;

    CustomInitProgressDialog(QWidget* parent, InitFunc initFunc)
        : QProgressDialog(parent) {
        initFunc(this);
    }
    ~CustomInitProgressDialog() {
        disconnect();
        close();
    }
};

class EmulatorQtWindow final : public QFrame {
    Q_OBJECT

public:
    using Ptr = std::shared_ptr<EmulatorQtWindow>;

private:
    explicit EmulatorQtWindow(QWidget* parent = 0);

public:
    static void create();
    static EmulatorQtWindow* getInstance();
    static Ptr getInstancePtr();

    virtual ~EmulatorQtWindow();

    void setSharedMemoryRenderer(SharedMemoryRenderer* renderer);

    void queueQuitEvent();
    void focusOutEvent(QFocusEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void tabletEvent(QTabletEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusInEvent(QFocusEvent *event) override;
    void startThread(StartFunction f, int argc, char** argv);
    void stopThread();

    // In Qt, signals are normally events of interest that a class can emit,
    // which can be hooked up to arbitrary slots. Here we use this mechanism for
    // a different purpose: it's to allow the QEMU thread to request an
    // operation be performed on the Qt thread; Qt allows signals to be emitted
    // from any thread. When used in this fashion, the signal is queued and
    // handled asyncrhonously.
    //
    // Since we sometimes will call these signals from Qt's thread as well, we
    // can't use BlockingQueuedConnections for these signals, since this
    // connection type will deadlock if called from the same thread. For that
    // reason, we use a normal non-blocking connection type, and allow all of
    // the signals to pass an optional semaphore that will be released by the
    // slot when it is done processing. If you want to block on the completion
    // of the signal, simply pass in the semaphore to the signal and acquire it
    // after the call returns. If you're passing in pointers to data structures
    // that could change or go away, you will need to make sure you block to
    // maintain the integrity of the data while the signal runs.
    //
    // TODO: allow nonblocking calls to these signals by having the signal take
    // ownership of object pointers. This would allow QEMU to do things like
    // update the screen without blocking, which would make it run faster.

signals:
    void blit(SkinSurfaceBitmap* src,
              QRect srcRect,
              SkinSurfaceBitmap* dst,
              QPoint dstPos,
              QPainter::CompositionMode op,
              QSemaphore* semaphore = NULL);
    void fill(SkinSurface* s,
              QRect rect,
              QColor color,
              QSemaphore* semaphore = NULL);
    void getDevicePixelRatio(double* out_dpr, QSemaphore* semaphore = NULL);
    void getScreenDimensions(QRect* out_rect, QSemaphore* semaphore = NULL);
    void getFramePos(int* x, int* y, QSemaphore* semaphore = NULL);
    void windowHasFrame(bool* outValue, QSemaphore* semaphore = NULL);
    void getWindowPos(int* x, int* y, QSemaphore* semaphore = NULL);
    void getWindowSize(int* w, int* h, QSemaphore* semaphore = NULL);
    void isWindowFullyVisible(bool* out_value, QSemaphore* semaphore = NULL);
    void isWindowOffScreen(bool* out_value, QSemaphore* semaphore = NULL);
    void releaseBitmap(SkinSurface* s, QSemaphore* sempahore = NULL);
    void requestClose(QSemaphore* semaphore = NULL);
    void requestUpdate(QRect rect, QSemaphore* semaphore = NULL);
    void setDeviceGeometry(QRect rect, QSemaphore* sempahore = NULL);
    void setWindowIcon(const unsigned char* data,
                       int size,
                       QSemaphore* semaphore = NULL);
    void setWindowPos(int x, int y, QSemaphore* semaphore = NULL);
    void resizableConfigChanged(int configId);
    void setWindowSize(int w, int h, QSemaphore* semaphore = NULL);
    void paintWindowOverlayForResize(int mouseX,
                                     int mouseY,
                                     QSemaphore* semaphore = NULL);
    void setWindowOverlayForResize(int whichCorner,
                                   QSemaphore* semaphore = NULL);
    void clearWindowOverlay(QSemaphore* semaphore = NULL);
    void setWindowCursorResize(int whichCorner, QSemaphore* semaphore = NULL);
    void setWindowCursorNormal(QSemaphore* semaphore = NULL);
    void setTitle(QString title, QSemaphore* semaphore = NULL);
    void showWindow(SkinSurface* surface,
                    QRect rect,
                    QSemaphore* semaphore = NULL);

    void runOnUiThread(RunOnUiThreadFunc f, QSemaphore* semaphore = NULL);
    void updateRotation(SkinRotation rotation);
    void layoutChanged(SkinRotation rot);

    void showMessage(QString text, Ui::OverlayMessageType icon, int timeoutMs);
    void showMessageWithDismissCallback(QString text,
                                        Ui::OverlayMessageType icon,
                                        QString dismissText,
                                        RunOnUiThreadFunc func,
                                        int timeoutMs);

    void showVirtualSceneControls(bool show);
    void updateMultiDisplayPage(int id);

public:
    void handleTouchPoints(const QTouchEvent& touchEvents,
                           uint32_t displayId = 0);
    void pollEvent(SkinEvent* event, bool* hasEvent);

    WId getWindowId();
    // Where the guest screen's area starts in the window of getWindowId(), in
    // logical pixels (see skin_winsys_get_content_offset).
    QPoint contentOffset() const;

    android::emulation::AdbInterface* getAdbInterface() const;
    void startSafeShutdown();
    void hideForSafeShutdown();
    void forceCloseNow();
    bool safeShutdownInProgress() const;
    // Windows ending the session: WM_QUERYENDSESSION (returns whether to
    // refuse it) and WM_ENDSESSION (|ending| is its wParam).
    bool queryEndSession(bool critical);
    void endSession(bool ending);
    EmulatorContainer* containerWindow();
    QSize guestAreaSize() const;
    bool isInZoomMode() const;
    bool isWindowMinimized() const { return mWindowIsMinimized; }
    ToolWindow* toolWindow() const;
    CarClusterWindow* carClusterWindow() const;
    CarClusterConnector* carClusterConnector() const;

    void showZoomIfNotUserHidden();
    QSize containerSize() const;
    QRect deviceGeometry() const;

    void doResize(const QSize& size, bool isKbdShortcut = false);
    void resizeAndChangeAspectRatio(bool isFolded);
    void resizeAndChangeAspectRatio(int x,
                                    int y,
                                    int w,
                                    int h,
                                    bool ignoreOrientation = false);
    void handleMouseEvent(SkinEventType type,
                          SkinMouseButtonType button,
                          const QPointF& pos,
                          const QPointF& gPos,
                          bool skipSync = false,
                          bool sendRelativeMouseCoordinates = true);
    void handlePenEvent(SkinEventType type,
                        const QTabletEvent* event,
                        bool skipSync = false);
    void handleMouseWheelEvent(int delta, Qt::Orientation orientation);
    void handleKeyEvent(SkinEventType type, const QKeyEvent& event);

    // Host IME text mode: while the guest input method (ModAVD Host Input)
    // reports an editable text field, typed text and composition from the
    // host's own input method go to it over the modavd_ime pipe. UI thread.
    bool isHostImeTextMode() const { return mHostImeTextMode; }
    void handleInputMethodEvent(QInputMethodEvent* event);
    // An invalid value means the query is not the host IME's to answer.
    QVariant hostImeQuery(Qt::InputMethodQuery query) const;
    // Keycode forwarding: whether the native event filter leaves a WM_KEYDOWN
    // to the key event, which then decides whether it still goes to the
    // guest. True in text mode, and for a key whose press went to the host
    // IME or was dropped when text mode started.
    bool hostImeDefersNativeKey(quint32 nativeScancode) const;
    // A WM_KEYDOWN or WM_SYSKEYDOWN to this window that the native event
    // filter saw, except one the host input method took: whether the key was
    // up before it, whether it is a modifier, and, with keycode forwarding,
    // the scancode to forward and whether the filter sent it to the guest
    // already or left it to the key event.
    void hostImeNativeKeyDown(quint32 nativeScancode,
                              bool newPress,
                              bool modifier,
                              quint32 forwardScancode,
                              bool forwarded,
                              bool deferred);
    // A WM_KEYUP or WM_SYSKEYUP; |takenByIme| when the host input method
    // took it (VK_PROCESSKEY in text mode), and Qt drops it.
    void hostImeNativeKeyUp(quint32 nativeScancode, bool takenByIme);
    // The native window that gets this window's key messages.
    WId hostImeKeyWindow() const { return mContainer.effectiveWinId(); }

    void panHorizontal(bool left);
    void panVertical(bool up);
    void queueSkinEvent(SkinEvent event);
    void recenterFocusPoint();
    void saveZoomPoints(const QPoint& focus, const QPoint& viewportFocus);
    void scaleDown();
    void scaleUp();
    void fixScale();
    void screenshot();
    void setFrameAlways(bool showFrame);
    void setOnTop(bool onTop);
    void setIgnoreWheelEvent(bool ignore);
    void setDisablePinchToZoom(bool disabled);
    void setPauseAvdWhenMinimized(bool ignore);
    void simulateKeyPress(int keyCode, int modifiers);
    void simulateScrollBarChanged(int x, int y);
    void setDisplayRegion(int xOffset, int yOffset, int width, int height);
    void setDisplayRegionAndUpdate(int xOffset,
                                   int yOffset,
                                   int width,
                                   int height);
    void simulateSetScale(double scale);
    void simulateSetZoom(double zoom);
    void simulateWindowMoved(const QPoint& pos);
    void simulateZoomedWindowResized(const QSize& size);
    void toggleZoomMode();
    void zoomIn();
    void zoomIn(const QPoint& focus, const QPoint& viewportFocus);
    void zoomOut();
    void zoomOut(const QPoint& focus, const QPoint& viewportFocus);
    void zoomReset();
    void zoomTo(const QPoint& focus, const QSize& rectSize);
    int getTopTransparency() { return mSkinGapTop; }
    int getRightTransparency() { return mSkinGapRight; }
    int getBottomTransparency() { return mSkinGapBottom; }
    int getLeftTransparency() { return mSkinGapLeft; }
    bool getMultiDisplay(uint32_t id,
                         int32_t* x,
                         int32_t* y,
                         uint32_t* w,
                         uint32_t* h,
                         uint32_t* dpi,
                         uint32_t* flag,
                         bool* enabled);
    int switchMultiDisplay(bool enabled,
                           uint32_t id,
                           int32_t x,
                           int32_t y,
                           uint32_t width,
                           uint32_t height,
                           uint32_t dpi,
                           uint32_t flag);
    bool getMonitorRect(uint32_t* width, uint32_t* height);
    void setNoSkin();
    void setFoldedSkin();
    void restoreSkin();
    bool multiDisplayParamValidate(uint32_t id,
                                   uint32_t w,
                                   uint32_t h,
                                   uint32_t dpi,
                                   uint32_t flag);
    void updateUIMultiDisplayPage(uint32_t id);
    void setUIDisplayRegion(int x,
                            int y,
                            int w,
                            int h,
                            bool ignoreOrientation = false);
    bool addMultiDisplayWindow(uint32_t id, bool add, uint32_t w, uint32_t h);
    bool paintMultiDisplayWindow(uint32_t id, uint32_t texture);
    void setRelativeMouseCoordMode(bool state);
    const char* releaseMouseShortcutName() const;
    bool isReleaseMouseShortcut(SkinEventType type, const QKeyEvent& event) const;
    void updateWindowTitle();

#ifdef __APPLE__
    // b/476267783: On Mac, override the default Qt methods to hide cursors
    // using the native NSCursor API when appropriate.
    void grabMouse(const QCursor& cursor);
    void releaseMouse();
#endif

    static bool sClosed;

public slots:
    void rotateSkin(SkinRotation rot);

private slots:
    void slot_adbWarningMessageAccepted();
    void slot_nestedWarningMessageAccepted();
    void slot_blit(SkinSurfaceBitmap* src,
                   QRect srcRect,
                   SkinSurfaceBitmap* dst,
                   QPoint dstPos,
                   QPainter::CompositionMode op,
                   QSemaphore* semaphore = NULL);
    void slot_clearInstance();
    void slot_fill(SkinSurface* s,
                   QRect rect,
                   QColor color,
                   QSemaphore* semaphore = NULL);
    void slot_getDevicePixelRatio(double* out_dpr,
                                  QSemaphore* semaphore = NULL);
    void slot_getScreenDimensions(QRect* out_rect,
                                  QSemaphore* semaphore = NULL);
    void slot_getFramePos(int* x, int* y, QSemaphore* semaphore = NULL);
    void slot_windowHasFrame(bool* outValue, QSemaphore* semaphore = NULL);
    void slot_getWindowPos(int* x, int* y, QSemaphore* semaphore = NULL);
    void slot_getWindowSize(int* w, int* h, QSemaphore* semaphore = NULL);
    void slot_isWindowFullyVisible(bool* out_value,
                                   QSemaphore* semaphore = NULL);
    void slot_isWindowOffScreen(bool* out_value, QSemaphore* semaphore = NULL);
    void slot_releaseBitmap(SkinSurface* s, QSemaphore* sempahore = NULL);
    void slot_requestClose(QSemaphore* semaphore = NULL);
    void slot_requestUpdate(QRect rect, QSemaphore* semaphore = NULL);
    void slot_setDeviceGeometry(QRect rect, QSemaphore* semaphore = NULL);
    void slot_setWindowIcon(const unsigned char* data,
                            int size,
                            QSemaphore* semaphore = NULL);
    void slot_setWindowPos(int x, int y, QSemaphore* semaphore = NULL);
    void slot_setWindowSize(int w, int h, QSemaphore* semaphore = NULL);
    void slot_paintWindowOverlayForResize(int mouseX,
                                          int mouseY,
                                          QSemaphore* semaphore = NULL);
    void slot_clearWindowOverlay(QSemaphore* semaphore = NULL);
    void slot_setWindowOverlayForResize(int whichCorner,
                                        QSemaphore* semaphore = NULL);
    void slot_setWindowCursorResize(int whichCorner,
                                    QSemaphore* semaphore = NULL);
    void slot_setWindowCursorNormal(QSemaphore* semaphore = NULL);
    void slot_setWindowTitle(QString title, QSemaphore* semaphore = NULL);
    void slot_showWindow(SkinSurface* surface,
                         QRect rect,
                         QSemaphore* semaphore = NULL);
    void slot_runOnUiThread(RunOnUiThreadFunc f, QSemaphore* semaphore = NULL);
    void slot_updateRotation(SkinRotation rotation);

    void slot_horizontalScrollChanged(int value);
    void slot_verticalScrollChanged(int value);

    void slot_scrollRangeChanged(int min, int max);

    void slot_startupTick();

    void slot_avdArchWarningMessageAccepted();
    void slot_gpuWarningMessageAccepted();

    void slot_installCanceled();
    void slot_adbPushCanceled();

    void slot_showMessage(QString text,
                          Ui::OverlayMessageType icon,
                          int timeoutMs);
    void slot_showMessageWithDismissCallback(
            QString text,
            Ui::OverlayMessageType icon,
            QString dismissText,
            Ui::OverlayChildWidget::DismissFunc func,
            int timeoutMs);
    void slot_updateGuestScreen(const QImage& frame);

public slots:
    // Here are conventional slots that perform interesting high-level functions
    // in the emulator. These can be hooked up to signals from UI elements or
    // called independently.
    void raise();
    void setForwardShortcutsToDevice(int index);
    void show();
    void showMinimized();
    void wheelScrollTimeout();
    void trackpadAggregateTimeout();

    void onScreenConfigChanged();
    void onScreenChanged(QScreen* newScreen);
    bool event(QEvent* ev) override;  // Used to resume the MV on un-minimize

public:
    bool isMainThreadRunning() const {
        return mMainLoopThread && mMainLoopThread->isRunning();
    }

    void initializeStreamer(std::string_view shm_handle,
                            StreamTransport transport = StreamTransport::MMAP);

signals:
    /**
     * @brief Emitted when a new frame has been rendered.
     *
     * @param frame A QImage containing the new frame.
     */
    void frameReady(const QImage& frame);

private:
    std::unique_ptr<SharedMemoryRenderer> mSharedMemoryRenderer;
    std::unique_ptr<SharedStreamEmulator> mStreamer;
    StreamTransport mTransportType = StreamTransport::MMAP;
    std::atomic<uint32_t> mActiveDisplayId{0};
    QPixmap mScreenMaskPixmap;
    QPixmap mRotatedScreenMaskPixmap;
    bool mScreenMaskChanged = true;
    void updateScreenMask();

    static const std::string_view kRemoteDownloadsDir;
    static const std::string_view kRemoteDownloadsDirApi10;
    // When the main window appears, close the "Starting..."
    // pop-up, if it was displayed.
    void showEvent(QShowEvent* event) override;

    // These are all called on first window show and
    // depend on AVD info, GPU info, etc.,
    // so they must be collected before the window shows up.
    void checkAdbVersionAndWarn();
    void showWin32DeprecationWarning();
    void showAvdArchWarning();
    void checkShouldShowGpuWarning();
    void showGpuWarning();
    void checkNestedAndWarn();
    void displayCheckWarnings();

    bool mouseInside();
    SkinMouseButtonType getSkinMouseButton(const QMouseEvent* event) const;

    int generateModData(const Qt::KeyboardModifiers& eventModifiers);
    void forwardKeyEventToEmulator(SkinEventType type, const QKeyEvent& event);
    void forwardGenericEventToEmulator(int type, int code, int value);

    void onHostImeState(bool connected,
                        bool active,
                        uint32_t inputType,
                        bool hasCursor,
                        QRect guestCursor,
                        unsigned changes);
    void setHostImeTextMode(bool on);
    bool hostImeKeyPress(const QKeyEvent& event);
    bool hostImeKeyRelease(const QKeyEvent& event);
    bool hostImeSendKey(const QKeyEvent& event);
    struct HostImeNativeKey;
    void hostImeExpireNativeKeys();
    bool hostImeTakeNativeKey(const QKeyEvent& event, HostImeNativeKey* key);
    void hostImeForwardNativeKey(quint32 scancode);
    struct HostImeKey;
    bool hostImeKeyIsDown(const HostImeKey& key) const;
    void hostImeReleaseRawKey(const HostImeKey& key);
    void hostImeUpdateCursor();
    QRect hostImeCursorRect() const;

    void maskWindowFrame();
    // Sizes the window for a guest screen that went from |guestBefore| to
    // |guest|, or leaves the window as it is and has the guest screen fitted
    // into it.
    void resizeContainerForGuest(const QSize& guest, const QSize& guestBefore);
    bool hasFrame() const;
    bool hasSkin() const;

    void runAdbInstall(const QString& path);
    void installDone(android::emulation::ApkInstaller::Result result,
                     std::string_view errorString);

    static const int kPushProgressBarMax;
    void runAdbPush(const QList<QUrl>& urls);
    void adbPushProgress(double progress, bool done);
    void adbPushDone(std::string_view filePath,
                     android::emulation::FilePusher::Result result);

    void runAdbShellPowerDownAndQuit();
    void requestGuestPowerOff();
    void requestInitPowerOff();
    void startSafeShutdownSync();
    void startSessionEndSync();
    void setVisibleExtent(QBitmap bitMap);
    Qt::CursorShape getCursorShape(bool mouseGrabbed);

    android::base::Looper* mLooper;
    QTimer mStartupTimer;
    bool mStartupDone = false;

    SkinSurface* mBackingSurface;

    QPixmap mGuestScreenPixmap;
    QPixmap mScaledBackingImage;
    bool mBackingBitmapChanged = true;

    std::deque<SkinEvent> mSkinEventQueue;
    std::mutex mSkinEventQueueMtx;

    // Snapshot state
    bool mShouldShowSnapshotModalOverlay = false;
    android::base::Lock mSnapshotStateLock;

    ToolWindow* mToolWindow;
    CarClusterWindow* mCarClusterWindow;
    CarClusterConnector* mCarClusterConnector;

    QRect mDeviceGeometry;

    std::unordered_set<int> mHeldModifiers;

    // Host IME text mode state, see isHostImeTextMode().
    int mHostImeCallbackId = 0;
    bool mHostImeTextMode = false;
    bool mHostImeResetting = false;  // inside QInputMethod::reset()
    bool mHostImeComposing = false;  // the guest has composing text from us
    uint32_t mHostImeInputType = 0;
    bool mHostImeHasCursor = false;
    QRect mHostImeGuestCursor;  // guest display pixels
    // The path each key's latest press took, by key: its auto-repeats, a
    // press Qt reports again after a modifier changes, and its release
    // follow it. A new press decides afresh, so an entry whose release never
    // came (Qt drops some) does no harm. Cleared on focus loss.
    enum class HostImePath : uint8_t {
        Ime,      // went to the host IME instead of the guest
        Raw,      // went the usual way
        Dropped,  // went the usual way and was released when text mode
                  // started; the rest of the press goes nowhere
    };
    struct HostImeKey {
        HostImePath path;
        int key;
        quint32 nativeScanCode;
        quint32 nativeVirtualKey;
    };
    std::unordered_map<quint32, HostImeKey> mHostImeHeld;
    // Windows: scancodes down by WM_KEYDOWN / WM_KEYUP, and the WM_KEYDOWNs
    // whose key events have not come yet. Qt delivers key events later,
    // maybe after more messages, and none for some WM_KEYDOWNs.
    std::unordered_set<quint32> mHostImeNativeDown;
    struct HostImeNativeKey {
        quint32 nativeScancode;
        quint32 forwardScancode;
        qint64 timeMs;
        bool newPress;           // the key was up before
        bool forwarded;          // keycode forwarding sent it already
        bool deferred;           // keycode forwarding left it to us
        bool releaseTakenByIme;  // the input method took its release
    };
    std::deque<HostImeNativeKey> mHostImeNativeKeys;
    // Set by hostImeKeyPress() for keyPressEvent(): the key went the usual
    // way, and the input method took its release.
    bool mHostImeReleaseAfterPress = false;

    bool mMouseGrabbed = false;
    bool mMouseRepositioning = false;
    bool mMouseRepositionFinished = false;
    bool mPromptMouseRestoreMessageBox = true;

    // Display blank cursor when mouse is grabbed.
    static constexpr Qt::CursorShape DEFAULT_MOUSE_GRABBED_MODE_CURSOR =
            Qt::BlankCursor;
    // Display default arrow cursor when mouse is not grabbed.
    static constexpr Qt::CursorShape DEFAULT_MOUSE_NORMAL_MODE_CURSOR =
            Qt::ArrowCursor;
    // If dual-mode driver is enabled in default mode, display the guest cursor
    // by default.
    static constexpr Qt::CursorShape DUAL_MODE_MOUSE_DEFAULT_CURSOR_DISPLAY_GUEST_MODE =
            Qt::BlankCursor;
    // If dual-mode driver is enabled in default mode, display the host cursor
    // if DualModeMouseDisplayHostCursor feature is enabled.
    static constexpr Qt::CursorShape DUAL_MODE_MOUSE_DEFAULT_CURSOR_DISPLAY_HOST_MODE =
            Qt::ArrowCursor;
    // If dual-mode driver is enabled in relative coordinates mode, display the
    // alternate cursor which shows head movement.
    static constexpr Qt::CursorShape DUAL_MODE_MOUSE_RELATIVE_MODE_CURSOR =
            Qt::CrossCursor;
    // Window flags to use for frameless and framed appearance

    static constexpr Qt::WindowFlags FRAMELESS_WINDOW_FLAGS =
            (Qt::Window | Qt::NoDropShadowWindowHint | Qt::FramelessWindowHint);
    static constexpr Qt::WindowFlags FRAMED_WINDOW_FLAGS =
            (Qt::Window | Qt::WindowTitleHint
#ifndef __APPLE__
             | Qt::CustomizeWindowHint
#endif
            );

    static constexpr Qt::WindowFlags FRAME_WINDOW_FLAGS_MASK =
            (FRAMELESS_WINDOW_FLAGS | FRAMED_WINDOW_FLAGS);

    QPointF mFocus;
    QPoint mViewportFocus;
    double mZoomFactor;
    bool mInZoomMode;
    bool mNextIsZoom;
    bool mForwardShortcutsToDevice;
    QPoint mPrevMousePosition;
    // How many rows or columns of the device skin are
    // tranparent on the four sides?
    int mSkinGapTop;
    int mSkinGapRight;
    int mSkinGapBottom;
    int mSkinGapLeft;

    int mScrollOverflowXInDegrees = 0;
    int mScrollOverflowYInDegrees = 0;

    MainLoopThread* mMainLoopThread;

    using OnDemandMessageBox =
            android::base::MemberOnDemandT<QMessageBox,
                                           QMessageBox::Icon,
                                           QString,
                                           QString,
                                           QMessageBox::StandardButton,
                                           QWidget*>;

    OnDemandMessageBox mWin32WarningBox;
    OnDemandMessageBox mAvdWarningBox;
    OnDemandMessageBox mGpuWarningBox;
    OnDemandMessageBox mAdbWarningBox;
    OnDemandMessageBox mNestedWarningBox;

    // First-show related warning messages state
    bool mGpuBlacklisted = false;
    bool mHasForcedRenderer = false;
    bool mShouldShowGpuWarning = false;
    bool mFirstShowWindowCall = true;
    bool mFirstShowEvent = true;
    // The room the guest screen was last asked to fit into again; see
    // resizeContainerForGuest().
    QSize mRefitGuestArea;
    // Some events may arrive after closing the main window (e.g. async screen
    // redraw requests); we want to ignore those when exiting.
    bool mClosed = false;
    // slot_clearInstance has run: Qt is quitting and the tool window is gone.
    bool mClearedForQuit = false;

    EventCapturer mEventCapturer;
    std::shared_ptr<UIEventRecorder<android::base::CircularBuffer>>
            mEventLogger;

    std::shared_ptr<android::qt::UserActionsCounter> mUserActionsCounter;
    android::base::MemberOnDemandT<android::emulation::AdbInterface*,
                                   android::emulation::AdbInterface*>
            mAdbInterface;
    android::emulation::AdbCommandPtr mApkInstallCommand;
    android::base::MemberOnDemandT<android::emulation::ApkInstaller,
                                   android::emulation::AdbInterface*>
            mApkInstaller;
    android::base::MemberOnDemandT<
            android::emulation::FilePusher,
            android::emulation::AdbInterface*,
            android::emulation::FilePusher::ResultCallback,
            android::emulation::FilePusher::ProgressCallback>
            mFilePusher;

    using OnDemandProgressDialog =
            android::base::MemberOnDemandT<CustomInitProgressDialog,
                                           QWidget*,
                                           CustomInitProgressDialog::InitFunc>;
    OnDemandProgressDialog mInstallDialog;
    OnDemandProgressDialog mPushDialog;

    bool mIgnoreWheelEvent = false;
    bool mDisablePinchToZoom = false;
    bool mPauseAvdWhenMinimized = false;
    QTimer mWheelScrollTimer;
    QTimer mTrackpadAggregateTimer;
    std::unique_ptr<SwipeGesture> mSwipeGesture;
    bool mStartedAdbStopProcess;

    enum class SafeShutdownState {
        Idle,
        StoppingGuest,
        Syncing,
        Forced,
    };
    SafeShutdownState mSafeShutdownState = SafeShutdownState::Idle;
    QTimer mSafeShutdownTimeout;
    bool mHiddenForSafeShutdown = false;
#ifdef _WIN32
    bool mSessionEndBlocked = false;
#endif

    bool mSkinRemoved{false};
    bool mFrameAlways;  // true = no floating emulator
    bool mPreviouslyFramed = false;
    bool mHaveBeenFrameless;
    unsigned int mHardRefreshCountDown = 0;
    SkinRotation mOrientation;  // Rotation of the main window
    bool mWindowIsMinimized = false;
    bool mRelativeMouseCoordMode = true;
    QString mBaseWindowTitle;

#ifdef __APPLE__
    // See grabMouse/releaseMouse.
    bool mIsCursorHidden = false;
#endif

    QScreen* mCurrentScreen = nullptr;
    SkinEvent createSkinEventScreenChanged();

    android::metrics::PeriodicReporter::TaskToken mMetricsReportingToken;
    void saveMultidisplayToConfig();

    /* Current values of Pen event data. */
    int mPenPosX = 0;
    int mPenPosY = 0;
    bool mPenButtonPressed = false;

    /* State of pen event type translation state machine. */
    TouchState mPenTouchState = TouchState::NOT_TOUCHING;
    /* State of mouse event type translation state machine. */
    TouchState mMouseTouchState = TouchState::NOT_TOUCHING;

    SkinEventType translatePenEventType(SkinEventType type,
                                        Qt::MouseButton button,
                                        Qt::MouseButtons buttons);
    SkinEventType translateMouseEventType(SkinEventType type,
                                          Qt::MouseButton button,
                                          Qt::MouseButtons buttons);
    int tiltToRotation(int xTiltDeg, int yTiltDeg);
    int penOrientation(int rotation);
    std::map<uint32_t, std::unique_ptr<MultiDisplayWidget>> mMultiDisplayWindow;
    // Putting these last because they both are constructed using *this pointer.
    EmulatorContainer mContainer;
    EmulatorOverlay mOverlay;
};

class SkinSurfaceBitmap {
    DISALLOW_COPY_AND_ASSIGN(SkinSurfaceBitmap);

public:
    SkinSurfaceBitmap(int w, int h);
    SkinSurfaceBitmap(const char* path);
    SkinSurfaceBitmap(const unsigned char* data, int size);
    SkinSurfaceBitmap(const SkinSurfaceBitmap& other, int rotation, int blend);
    ~SkinSurfaceBitmap();

    QSize size() const;

    void fill(const QRect& area, const QColor& color);
    void drawFrom(SkinSurfaceBitmap* what,
                  QPoint where,
                  const QRect& area,
                  QPainter::CompositionMode op);

    QImage& get();

private:
    void readImage();
    void applyPendingTransformations();
    bool hasPendingTransformations() const;
    void resetReader();

    QImage image;
    QImageReader reader;
    QSize cachedSize;
    // |pendingRotation| represents the amount of rotation to apply to |image|
    // before accessing it.
    int pendingRotation = SKIN_ROTATION_0;
    // |pendingBlend| is a number in 0..256 ( == SKIN_BLEND_FULL) range, which
    // is effectively an alpha blend value to apply to |image| before accessing
    // its pixels. Default value 256 is the same as alpha = 1.0, 128 is 0.5 and
    // 0 is totally transparent.
    int pendingBlend = SKIN_BLEND_FULL;
};

struct SkinSurface {
    int id;
    int w, h;
    int isRound;
    EmulatorQtWindow::Ptr window;
    SkinSurfaceBitmap* bitmap;
};
