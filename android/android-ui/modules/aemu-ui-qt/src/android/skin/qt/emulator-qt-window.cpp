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

#include "android/skin/qt/emulator-qt-window.h"

#include "aemu/base/Optional.h"
#include "aemu/base/async/ThreadLooper.h"
#include "aemu/base/files/PathUtils.h"
#include "aemu/base/memory/LazyInstance.h"
#include "aemu/base/memory/ScopedPtr.h"
#include "aemu/base/synchronization/Lock.h"
#include "aemu/base/system/Win32Utils.h"
#include "aemu/base/threads/Async.h"
#include "android/android.h"
#include "android/avd/info.h"
#include "android/base/system/System.h"
#include "android/cmdline-option.h"
#include "android/console.h"
#include "android/cpu_accelerator.h"
#include "android/crashreport/CrashReporter.h"
#include "android/emu/check/include/android/emulation/compatibility_check.h"
#include "android/emulation/AdbHostServer.h"
#include "android/emulation/ModavdImePipe.h"
#include "android/emulation/compatibility_check.h"
#include "android/emulation/control/globals_agent.h"
#include "android/emulation/control/user_event_agent.h"
#include "android/emulator-window.h"
#include "android/hw-sensors.h"
#include "android/metrics/DependentMetrics.h"
#include "android/metrics/PeriodicReporter.h"
#include "android/metrics/studio_stats_wrapper.pb.h"
#include "android/multitouch-screen.h"
#include "android/opengl/gpuinfo.h"
#include "android/skin/EmulatorSkin.h"
#include "android/skin/event.h"
#include "android/skin/keycode.h"
#include "android/skin/qt/FramelessDetector.h"
#include "android/skin/qt/QtLooper.h"
#include "android/skin/qt/SharedMemoryRenderer.h"
#include "android/skin/qt/SharedStreamEmulator.h"
#include "android/skin/qt/car-cluster-window.h"
#include "android/skin/qt/event-serializer.h"
#include "android/skin/qt/extended-pages/car-cluster-connector/car-cluster-connector.h"
#include "android/skin/qt/extended-pages/common.h"
#include "android/skin/qt/extended-pages/microphone-page.h"
#include "android/skin/qt/extended-pages/multi-display-page.h"
#include "android/skin/qt/extended-pages/settings-page.h"
#include "android/skin/qt/extended-pages/snapshot-page.h"
#include "android/skin/qt/extended-pages/telephony-controller.h"
#include "android/skin/qt/extended-pages/telephony-page.h"
#include "android/skin/qt/multi-display-widget.h"
#include "android/skin/qt/native-keyboard-event-handler.h"
#include "android/skin/qt/qt-keycode.h"
#include "android/skin/qt/qt-settings.h"
#include "android/skin/qt/screen-mask.h"
#include "android/skin/qt/winsys-qt.h"
#include "android/skin/rect.h"
#include "android/skin/winsys.h"
#include "android/snapshot/Snapshotter.h"
#include "android/test/checkboot.h"
#include "android/ui-emu-agent.h"
#include "android/utils/eintr_wrapper.h"
#include "android/utils/filelock.h"
#include "android/utils/x86_cpuid.h"
#include "ver/virtual_environment_renderer.h" // for ver_texture_utils_load_png
#include "android_modem_v2.h"
#include "emulator_controller.pb.h"
#include "host-common/FeatureControl.h"
#include "host-common/Features.h"
#include "host-common/MultiDisplay.h"
#include "host-common/crash-handler.h"
#include "host-common/feature_control.h"
#include "host-common/multi_display_agent.h"
#include "host-common/opengl/emugl_config.h"
#include "host-common/snapshot_common.h"

#define DEBUG 1

#if DEBUG
#include "android/utils/debug.h"
#define D(...) VERBOSE_PRINT(surface, __VA_ARGS__)
#else
#define D(...) ((void)0)
#endif

#include <QBitmap>
#include <QCheckBox>
#include <QCursor>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFontMetrics>
#include <QIcon>
#include <QInputDevice>
#include <QInputMethod>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSemaphore>
#include <QSettings>
#include <QTabletEvent>
#include <QToolTip>
#include <QTouchEvent>
#include <QWindow>
#include <Qt>
#include <QtCore>
#include <QtMath>

#ifdef _WIN32
#include <io.h>
#else
#ifndef _MSC_VER
#include <unistd.h>
#endif
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef __linux__
// This include needs to be after all the Qt includes
// because it defines macros/types that conflict with
// qt's own macros/types.
#elif defined(__APPLE__)
#include "android/skin/qt/mac-native-window.h"
#else  // windows
#include <windows.h>
#endif

using android::base::AutoLock;
using android::base::c_str;
using android::base::kNullopt;
using android::base::LazyInstance;
using android::base::Lock;
using android::base::makeOptional;
using android::base::PathUtils;
using android::base::ScopedCPtr;
using android::base::System;
using android::crashreport::CrashReporter;
using android::emulation::ApkInstaller;
using android::emulation::AvdCompatibility;
using android::emulation::AvdCompatibilityManager;
using android::emulation::FilePusher;
using android::emulation::ModavdImePipe;

// The adb callbacks of the lean toolbar's safe shutdown run on the
// AdbInterface looper and can complete after the window is gone (Qt tears it
// down once Android has powered off). They post to the window only while it
// is still alive; the destructor clears the pointer under the lock.
static Lock& safeShutdownLock() {
    static Lock lock;
    return lock;
}
static EmulatorQtWindow* sSafeShutdownWindow = nullptr;

namespace {

#define QT_TO_LINUX_KEY(x, y) {Qt::Key_##x, LINUX_KEY_##y}
#define QT_LINUX_SAME_KEY(x) QT_TO_LINUX_KEY(x, x)

const std::unordered_map<int, int> QT_TO_LINUX_KEYCODE_BASE{
        QT_TO_LINUX_KEY(Left, LEFT),
        QT_TO_LINUX_KEY(Right, RIGHT),
        QT_TO_LINUX_KEY(Up, UP),
        QT_TO_LINUX_KEY(Down, DOWN),
        QT_LINUX_SAME_KEY(0),
        QT_LINUX_SAME_KEY(1),
        QT_LINUX_SAME_KEY(2),
        QT_LINUX_SAME_KEY(3),
        QT_LINUX_SAME_KEY(4),
        QT_LINUX_SAME_KEY(5),
        QT_LINUX_SAME_KEY(6),
        QT_LINUX_SAME_KEY(7),
        QT_LINUX_SAME_KEY(8),
        QT_LINUX_SAME_KEY(9),
        QT_LINUX_SAME_KEY(F1),
        QT_LINUX_SAME_KEY(F2),
        QT_LINUX_SAME_KEY(F3),
        QT_LINUX_SAME_KEY(F4),
        QT_LINUX_SAME_KEY(F5),
        QT_LINUX_SAME_KEY(F6),
        QT_LINUX_SAME_KEY(F7),
        QT_LINUX_SAME_KEY(F8),
        QT_LINUX_SAME_KEY(F9),
        QT_LINUX_SAME_KEY(F10),
        QT_LINUX_SAME_KEY(F11),
        QT_LINUX_SAME_KEY(F12),
        QT_LINUX_SAME_KEY(A),
        QT_LINUX_SAME_KEY(B),
        QT_LINUX_SAME_KEY(C),
        QT_LINUX_SAME_KEY(D),
        QT_LINUX_SAME_KEY(E),
        QT_LINUX_SAME_KEY(F),
        QT_LINUX_SAME_KEY(G),
        QT_LINUX_SAME_KEY(H),
        QT_LINUX_SAME_KEY(I),
        QT_LINUX_SAME_KEY(J),
        QT_LINUX_SAME_KEY(K),
        QT_LINUX_SAME_KEY(L),
        QT_LINUX_SAME_KEY(M),
        QT_LINUX_SAME_KEY(N),
        QT_LINUX_SAME_KEY(O),
        QT_LINUX_SAME_KEY(P),
        QT_LINUX_SAME_KEY(Q),
        QT_LINUX_SAME_KEY(R),
        QT_LINUX_SAME_KEY(S),
        QT_LINUX_SAME_KEY(T),
        QT_LINUX_SAME_KEY(U),
        QT_LINUX_SAME_KEY(V),
        QT_LINUX_SAME_KEY(W),
        QT_LINUX_SAME_KEY(X),
        QT_LINUX_SAME_KEY(Y),
        QT_LINUX_SAME_KEY(Z),
        QT_TO_LINUX_KEY(Minus, MINUS),
        QT_TO_LINUX_KEY(Equal, EQUAL),
        QT_TO_LINUX_KEY(Backspace, BACKSPACE),
        QT_TO_LINUX_KEY(Home, HOME),
        QT_TO_LINUX_KEY(Escape, ESC),
        QT_TO_LINUX_KEY(Comma, COMMA),
        QT_TO_LINUX_KEY(Period, DOT),
        QT_TO_LINUX_KEY(Space, SPACE),
        QT_TO_LINUX_KEY(Slash, SLASH),
        QT_TO_LINUX_KEY(Return, ENTER),
        QT_TO_LINUX_KEY(Tab, TAB),
        QT_TO_LINUX_KEY(BracketLeft, LEFTBRACE),
        QT_TO_LINUX_KEY(BracketRight, RIGHTBRACE),
        QT_TO_LINUX_KEY(Backslash, BACKSLASH),
        QT_TO_LINUX_KEY(Semicolon, SEMICOLON),
        QT_TO_LINUX_KEY(Apostrophe, APOSTROPHE),
        QT_TO_LINUX_KEY(Delete, DELETE),
        // Qt treats "SHIFT + TAB" as "Backtab", just convert it back to
        // TAB.
        QT_TO_LINUX_KEY(Backtab, TAB),
        // LINUX_KEY_GREEN is mapped to LINUX_KEY_GRAVE in emulator system image
        // qwerty2.kl
        QT_TO_LINUX_KEY(QuoteLeft, GREEN),
};

const std::unordered_map<int, int> QT_TO_LINUX_KEYCODE_MODIFIERS{
        // QT only provides one signal for both left/right modifiers;
        // always send these as Left.
        QT_TO_LINUX_KEY(Control, LEFTCTRL),
        QT_TO_LINUX_KEY(Alt, LEFTALT),
        QT_TO_LINUX_KEY(Shift, LEFTSHIFT),
        QT_TO_LINUX_KEY(Meta, LEFTMETA),
};

static int convertKeyCode(int sym, bool& isModifier) {
    // Convert a Qt::Key_XXX code into the corresponding Linux keycode value.
    // On failure, return -1.

    isModifier = false;

    // TODO(b/286512287): KEY_ENTER is not recognized by Android TV system
    // image as what we expect it to be. Use KEY_CENTER instead.
    if (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
                AVD_TV &&
        sym == Qt::Key_Return) {
        return LINUX_KEY_CENTER;
    }

    auto res = QT_TO_LINUX_KEYCODE_BASE.find(sym);
    if (res != QT_TO_LINUX_KEYCODE_BASE.end()) {
        return res->second;
    }

    // Only Raw Input mode will send modifier keyup/keydowns.
    if (!android::featurecontrol::isEnabled(
                android::featurecontrol::QtRawKeyboardInput)) {
        return -1;
    }

    res = QT_TO_LINUX_KEYCODE_MODIFIERS.find(sym);
    if (res != QT_TO_LINUX_KEYCODE_MODIFIERS.end()) {
        isModifier = true;
        return res->second;
    }

    return -1;
}

static SkinEvent createMouseTrackingSkinEvent(SkinEventType eventType) {
    auto event = createSkinEvent(eventType);
    event.u.mouse.display_id = 0;
    return event;
}

// Host IME text mode: values of android.view.KeyEvent and
// android.text.InputType.
constexpr uint32_t kAndroidKeycodeA = 29;  // KEYCODE_A .. KEYCODE_Z = 54
constexpr uint32_t kAndroidMetaShiftOn = 0x1;
constexpr uint32_t kAndroidMetaAltOn = 0x2;
constexpr uint32_t kAndroidMetaCtrlOn = 0x1000;
constexpr uint32_t kInputTypeMaskClass = 0xf;
constexpr uint32_t kInputTypeMaskVariation = 0xff0;
constexpr uint32_t kInputTypeClassText = 0x1;
constexpr uint32_t kInputTypeClassNumber = 0x2;
constexpr uint32_t kInputTypeTextVariationPassword = 0x80;
constexpr uint32_t kInputTypeTextVariationVisiblePassword = 0x90;
constexpr uint32_t kInputTypeTextVariationWebPassword = 0xe0;
constexpr uint32_t kInputTypeNumberVariationPassword = 0x10;
// How long the native event filter's record of a WM_KEYDOWN waits for its key
// event.
constexpr qint64 kHostImeNativeKeyMs = 1000;
constexpr size_t kHostImeMaxNativeKeys = 64;

// The Android keycode a text-mode editing key sends, or 0.
static uint32_t hostImeEditingKeycode(int key) {
    switch (key) {
        case Qt::Key_Backspace:
            return 67;  // KEYCODE_DEL
        case Qt::Key_Delete:
            return 112;  // KEYCODE_FORWARD_DEL
        case Qt::Key_Return:
        case Qt::Key_Enter:
            return 66;  // KEYCODE_ENTER
        case Qt::Key_Tab:
        case Qt::Key_Backtab:  // Shift+Tab
            return 61;  // KEYCODE_TAB
        case Qt::Key_Left:
            return 21;  // KEYCODE_DPAD_LEFT
        case Qt::Key_Up:
            return 19;  // KEYCODE_DPAD_UP
        case Qt::Key_Right:
            return 22;  // KEYCODE_DPAD_RIGHT
        case Qt::Key_Down:
            return 20;  // KEYCODE_DPAD_DOWN
        case Qt::Key_Home:
            return 122;  // KEYCODE_MOVE_HOME
        case Qt::Key_End:
            return 123;  // KEYCODE_MOVE_END
        case Qt::Key_PageUp:
            return 92;  // KEYCODE_PAGE_UP
        case Qt::Key_PageDown:
            return 93;  // KEYCODE_PAGE_DOWN
        default:
            return 0;
    }
}

// Identifies a physical key from press to release; key() and text() can
// change in between when modifiers do.
static quint32 hostImeKeyId(const QKeyEvent& event) {
    return event.nativeScanCode() ? event.nativeScanCode()
                                  : (0x80000000u | quint32(event.key()));
}

static qint64 hostImeNowMs() {
    static QElapsedTimer timer;
    if (!timer.isValid()) {
        timer.start();
    }
    return timer.elapsed();
}

}  // namespace

// Make sure it is POD here
static LazyInstance<EmulatorQtWindow::Ptr> sInstance = LAZY_INSTANCE_INIT;

// static
const int EmulatorQtWindow::kPushProgressBarMax = 2000;
const std::string_view EmulatorQtWindow::kRemoteDownloadsDir =
        "/sdcard/Download/";

constexpr Qt::WindowFlags EmulatorQtWindow::FRAMED_WINDOW_FLAGS;
constexpr Qt::WindowFlags EmulatorQtWindow::FRAMELESS_WINDOW_FLAGS;
constexpr Qt::WindowFlags EmulatorQtWindow::FRAME_WINDOW_FLAGS_MASK;

// Gingerbread devices download files to the following directory (note the
// uncapitalized "download").
const std::string_view EmulatorQtWindow::kRemoteDownloadsDirApi10 =
        "/sdcard/download/";

// Place to tell everyone we're exiting
//
// Isn't a member of the window object, so no issues with racing to delete
bool EmulatorQtWindow::sClosed = false;

SkinSurfaceBitmap::SkinSurfaceBitmap(int w, int h) : cachedSize(w, h) {}

SkinSurfaceBitmap::SkinSurfaceBitmap(const char* path) : reader(path) {
    cachedSize = reader.size();
    // When loading png file from a file path, QT's QImage class will use its
    // own libpng which introduces unexpected dependencies on libz from system
    // lib. Thus, causing crash for certain png file. (b/127953242) To
    // workaround, we use an inplementation which is based on libpng built from
    // source and it doesn't introduce unexpected dependencies.
    // This code path will be executed iff the file format is png.
    if (PathUtils::extension(path) == ".png") {
        int width = 0, height = 0, formatBpp = 0;
        std::vector<uint8_t> imageBuffer;
        if (ver_texture_utils_load_png(reader.fileName().toStdString().c_str(),
                                       &width, &height, &formatBpp,
                                       &imageBuffer)) {
            QImage::Format format = (formatBpp == 3) ? QImage::Format_RGB888
                                                     : QImage::Format_RGBA8888;
            image = QImage(
                    reinterpret_cast<const unsigned char*>(imageBuffer.data()),
                    width, height, format);

            image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            resetReader();
        }
    }
}

SkinSurfaceBitmap::SkinSurfaceBitmap(const unsigned char* data, int size) {
    auto array =
            new QByteArray(QByteArray::fromRawData((const char*)data, size));
    auto buffer = new QBuffer(array);
    buffer->open(QIODevice::ReadOnly);
    reader.setDevice(buffer);
    cachedSize = reader.size();
}

SkinSurfaceBitmap::SkinSurfaceBitmap(const SkinSurfaceBitmap& other,
                                     int rotation,
                                     int blend)
    : image(other.image), cachedSize(other.cachedSize) {
    // Mix two alpha blend values to produce some average in the same 0..256
    // range as the original one. Round it up to minimize chances when we end up
    // with |pendingBlend| == 0.
    pendingBlend = (blend * other.pendingBlend + SKIN_BLEND_FULL - 1) /
                   SKIN_BLEND_FULL;
    pendingRotation = (rotation + other.pendingRotation) & 3;

    if (other.reader.device()) {
        if (other.reader.fileName().isEmpty()) {
            auto otherBuffer = qobject_cast<QBuffer*>(other.reader.device());
            auto array = new QByteArray(otherBuffer->buffer());
            auto buffer = new QBuffer(array);
            buffer->open(QIODevice::ReadOnly);
            reader.setDevice(buffer);
        } else {
            reader.setFileName(other.reader.fileName());
        }
    }
}

void SkinSurfaceBitmap::resetReader() {
    if (reader.device()) {
        if (auto buffer = qobject_cast<QBuffer*>(reader.device())) {
            delete &buffer->buffer();
            delete buffer;
        } else {
            reader.device()->moveToThread(QThread::currentThread());
        }
        reader.setDevice(nullptr);
    }
}

SkinSurfaceBitmap::~SkinSurfaceBitmap() {
    resetReader();
}

QSize SkinSurfaceBitmap::size() const {
    return (pendingRotation & 1) ? cachedSize.transposed() : cachedSize;
}

void SkinSurfaceBitmap::applyPendingTransformations() {
    if (!hasPendingTransformations()) {
        return;
    }

    readImage();
    if (pendingBlend == SKIN_BLEND_FULL) {
        if (pendingRotation == SKIN_ROTATION_180) {
            // can shortcut it with mirroring
            image = image.mirrored(true, true);
        } else {
            // a simple transformation is still enough here
            QTransform transform;
            transform.rotate(pendingRotation * 90);
            image = image.transformed(transform);
        }
    } else {
        QImage newImage(size(), QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&newImage);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.setOpacity(pendingBlend / double(SKIN_BLEND_FULL));
        painter.rotate(pendingRotation * 90);
        painter.drawImage(QPoint(), image);
        painter.end();
        image = newImage;
    }
    pendingBlend = SKIN_BLEND_FULL;
    pendingRotation = SKIN_ROTATION_0;
    cachedSize = image.size();
}

bool SkinSurfaceBitmap::hasPendingTransformations() const {
    return pendingRotation != SKIN_ROTATION_0 ||
           pendingBlend != SKIN_BLEND_FULL;
}

void SkinSurfaceBitmap::fill(const QRect& area, const QColor& color) {
    if (area.topLeft() == QPoint() && area.size() == size()) {
        // simple case - fill the whole image
        if (image.isNull() || (pendingRotation & 1)) {
            image = QImage(size(), QImage::Format_ARGB32_Premultiplied);
        }
        image.fill(color);
        resetReader();
        pendingBlend = SKIN_BLEND_FULL;
        pendingRotation = SKIN_ROTATION_0;
        cachedSize = image.size();
    } else {
        QPainter painter(&get());
        painter.fillRect(area, color);
    }
}

void SkinSurfaceBitmap::drawFrom(SkinSurfaceBitmap* what,
                                 QPoint where,
                                 const QRect& area,
                                 QPainter::CompositionMode op) {
    if (area.topLeft() == QPoint() && area.size() == size() &&
        what->get().size() == area.size() &&
        op == QPainter::CompositionMode_Source) {
        image = what->get();
        resetReader();
        pendingBlend = SKIN_BLEND_FULL;
        pendingRotation = SKIN_ROTATION_0;
        cachedSize = image.size();
    } else {
        QPainter painter(&get());
        painter.setCompositionMode(op);
        painter.drawImage(where, what->get(), area);
    }
}

QImage& SkinSurfaceBitmap::get() {
    readImage();
    applyPendingTransformations();
    return image;
}

void SkinSurfaceBitmap::readImage() {
    if (!image.isNull()) {
        return;
    }
    if (reader.device()) {
        reader.read(&image);
        if (image.format() != QImage::Format_ARGB32_Premultiplied) {
            image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }
        resetReader();
    } else {
        image = QImage(cachedSize, QImage::Format_ARGB32_Premultiplied);
        image.fill(0);
    }
}

SkinEvent EmulatorQtWindow::createSkinEventScreenChanged() {
    QRect screen_geo;
    getScreenDimensions(&screen_geo);
    double dpr;
    getDevicePixelRatio(&dpr);

    // On multi-monitor setups, x and y coordinates can have negative values, as
    // the coordinates are relative to the top-left corner (0, 0) of the primary
    // display.
    SkinEvent skin_event = createSkinEvent(kEventScreenChanged);
    skin_event.u.screen.x = screen_geo.x();
    skin_event.u.screen.y = screen_geo.y();
    skin_event.u.screen.w = screen_geo.width();
    skin_event.u.screen.h = screen_geo.height();
    skin_event.u.screen.dpr = dpr;
    D("%s: x=%d y=%d w=%d h=%d dpr=%f\n", __func__, skin_event.u.screen.x,
      skin_event.u.screen.y, skin_event.u.screen.w, skin_event.u.screen.h,
      skin_event.u.screen.dpr);
    return skin_event;
}

////////////////////////////////////////////////////////////////////////////////

void EmulatorQtWindow::create() {
    sInstance.get() = Ptr(new EmulatorQtWindow());
}

EmulatorQtWindow::EmulatorQtWindow(QWidget* parent)
    : QFrame(parent),
      mLooper(android::qt::createLooper()),
      mToolWindow(nullptr),
      mCarClusterWindow(nullptr),
      mCarClusterConnector(nullptr),
      mContainer(this),
      mOverlay(this, &mContainer),
      mZoomFactor(1.0),
      mInZoomMode(false),
      mNextIsZoom(false),
      mForwardShortcutsToDevice(false),
      mPrevMousePosition(0, 0),
      mSkinGapTop(0),
      mSkinGapRight(0),
      mSkinGapBottom(0),
      mSkinGapLeft(0),
      mMainLoopThread(nullptr),
      mWin32WarningBox(
              QMessageBox::Information,
              tr("Windows 32-bit Deprecation"),
              tr("We strongly recommend using the 64-bit version "
                 "of the Windows emulator; the 32-bit version is deprecated "
                 "and will be removed soon."),
              QMessageBox::Ok,
              this),
      mAvdWarningBox(QMessageBox::Information,
                     tr("Recommended AVD"),
                     tr("Running an x86 based Android Virtual Device (AVD) is "
                        "10x faster.<br/>"
                        "We strongly recommend creating a new AVD."),
                     QMessageBox::Ok,
                     this),
      mGpuWarningBox([this] {
          return std::make_tuple(
                  QMessageBox::Information, tr("GPU Driver Issue"),
                  tr("Your GPU driver information:\n\n"
                     "%1\n"
                     "Some users have experienced emulator stability issues "
                     "with this driver version.  As a result, we're "
                     "selecting a compatibility renderer.  Please check with "
                     "your manufacturer to see if there is an updated "
                     "driver available.")
                          .arg(QString::fromStdString(
                                  globalGpuInfoList().dump())),
                  QMessageBox::Ok, this);
      }),
      mAdbWarningBox(QMessageBox::Warning,
                     tr("Detected ADB"),
                     tr(""),
                     QMessageBox::Ok,
                     this),
      mNestedWarningBox(QMessageBox::Information,
                        tr("Emulator Running in Nested Virtualization"),
                        tr("Emulator is running using nested virtualization. "
                           "This is not recommended. It may not work at all. "
                           "And typically the performance is not quite good."),
                        QMessageBox::Ok,
                        this),

      mEventLogger(
              std::make_shared<UIEventRecorder<android::base::CircularBuffer>>(
                      &mEventCapturer,
                      1000)),
      mUserActionsCounter(new android::qt::UserActionsCounter(&mEventCapturer)),
      mAdbInterface([this] {
          return android::emulation::AdbInterface::createGlobalOwnThread();
      }),
      mApkInstaller([this] { return (*mAdbInterface); }),
      mFilePusher([this] {
          return std::make_tuple(
                  (*mAdbInterface),
                  [this](std::string_view filePath, FilePusher::Result result) {
                      runOnUiThread([this, filePath, result] {
                          adbPushDone(filePath, result);
                      });
                  },
                  [this](double progress, bool done) {
                      runOnUiThread([this, progress, done] {
                          adbPushProgress(progress, done);
                      });
                  });
      }),
      mInstallDialog(this,
                     [this](QProgressDialog* dialog) {
                         dialog->setWindowTitle(tr("APK Installer"));
                         dialog->setLabelText(tr("Installing APK..."));
                         dialog->setRange(0, 0);  // Makes it a "busy" dialog
                         dialog->setModal(true);
                         dialog->close();
                         QObject::connect(dialog, SIGNAL(canceled()), this,
                                          SLOT(slot_installCanceled()));
                     }),
      mPushDialog(this,
                  [this](QProgressDialog* dialog) {
                      dialog->setWindowTitle(tr("File Copy"));
                      dialog->setLabelText(tr("Copying files..."));
                      dialog->setRange(0, kPushProgressBarMax);
                      dialog->close();
                      QObject::connect(dialog, SIGNAL(canceled()), this,
                                       SLOT(slot_adbPushCanceled()));
                  }),
      mStartedAdbStopProcess(false),
      mHaveBeenFrameless(false) {
    qRegisterMetaType<QPainter::CompositionMode>();
    qRegisterMetaType<SkinRotation>();
    qRegisterMetaType<SkinGenericFunction>();
    qRegisterMetaType<RunOnUiThreadFunc>();
    qRegisterMetaType<Ui::OverlayMessageType>();
    qRegisterMetaType<Ui::OverlayChildWidget::DismissFunc>();

    if (getConsoleAgents()->settings->android_cmdLineOptions()->grpc_ui &&
        getConsoleAgents()->emu) {
        // For standalone gRPC UI clients (such as Fishtank), determine the initial window
        // orientation dynamically by querying the live physical model state over gRPC rather
        // than relying on static hardware config defaults.
        mOrientation = static_cast<SkinRotation>(getConsoleAgents()->emu->getRotation());
    } else if (EmulatorSkin::getInstance()->isPortrait()) {
        mOrientation = !strcasecmp(getConsoleAgents()
                                           ->settings->hw()
                                           ->hw_initialOrientation,
                                   "landscape")
                               ? SKIN_ROTATION_270
                               : SKIN_ROTATION_0;
    } else {
        // landscape
        mOrientation = !strcasecmp(getConsoleAgents()
                                           ->settings->hw()
                                           ->hw_initialOrientation,
                                   "landscape")
                               ? SKIN_ROTATION_0
                               : SKIN_ROTATION_90;
    }

    android::base::ThreadLooper::setLooper(mLooper, true);

    // TODO: Weird input lag with Qt will cause hang detector to trip and crash
    // the emulator, even though it works ok (but laggy) otherwise.
    // For now, disable Qt hang detection.
    // bug: 73723222
    // bug: 74520987
    // CrashReporter::get()->hangDetector().addWatchedLooper(mLooper);

    // Start a timer. If the main window doesn't
    // appear before the timer expires, show a
    // pop-up to let the user know we're still
    // working.
    QObject::connect(&mStartupTimer, &QTimer::timeout, this,
                     &EmulatorQtWindow::slot_startupTick);
    mStartupTimer.setSingleShot(true);
    mStartupTimer.setInterval(500);  // Half a second
    mStartupTimer.start();

    mBackingSurface = NULL;

    QSettings settings;
    mFrameAlways =
            FramelessDetector::isFramelessOk()
                    ? settings.value(Ui::Settings::FRAME_ALWAYS, false).toBool()
                    : true;
    if (ToolWindow::inWindowNav()) {
        mFrameAlways = true;
    }
    mPreviouslyFramed = mFrameAlways;

    mToolWindow = new ToolWindow(this, &mContainer, mEventLogger,
                                 mUserActionsCounter);

    if (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
        AVD_ANDROID_AUTO) {
        mCarClusterWindow = new CarClusterWindow(this, &mContainer);
        mCarClusterConnector = new CarClusterConnector(mCarClusterWindow);
    }

    this->setAcceptDrops(true);
    QObject::connect(this, &EmulatorQtWindow::showVirtualSceneControls,
                     mToolWindow, &ToolWindow::showVirtualSceneControls);

    QObject::connect(this, &EmulatorQtWindow::blit, this,
                     &EmulatorQtWindow::slot_blit);
    QObject::connect(this, &EmulatorQtWindow::fill, this,
                     &EmulatorQtWindow::slot_fill);
    QObject::connect(this, &EmulatorQtWindow::getDevicePixelRatio, this,
                     &EmulatorQtWindow::slot_getDevicePixelRatio);
    QObject::connect(this, &EmulatorQtWindow::getScreenDimensions, this,
                     &EmulatorQtWindow::slot_getScreenDimensions);
    QObject::connect(this, &EmulatorQtWindow::getFramePos, this,
                     &EmulatorQtWindow::slot_getFramePos);
    QObject::connect(this, &EmulatorQtWindow::windowHasFrame, this,
                     &EmulatorQtWindow::slot_windowHasFrame);
    QObject::connect(this, &EmulatorQtWindow::getWindowPos, this,
                     &EmulatorQtWindow::slot_getWindowPos);
    QObject::connect(this, &EmulatorQtWindow::getWindowSize, this,
                     &EmulatorQtWindow::slot_getWindowSize);
    QObject::connect(this, &EmulatorQtWindow::isWindowFullyVisible, this,
                     &EmulatorQtWindow::slot_isWindowFullyVisible);
    QObject::connect(this, &EmulatorQtWindow::isWindowOffScreen, this,
                     &EmulatorQtWindow::slot_isWindowOffScreen);
    QObject::connect(this, &EmulatorQtWindow::releaseBitmap, this,
                     &EmulatorQtWindow::slot_releaseBitmap);
    QObject::connect(this, &EmulatorQtWindow::requestClose, this,
                     &EmulatorQtWindow::slot_requestClose);
    mSafeShutdownTimeout.setSingleShot(true);
    mSafeShutdownTimeout.setInterval(15000);
    QObject::connect(&mSafeShutdownTimeout, &QTimer::timeout, this, [this] {
        if (mSafeShutdownState == SafeShutdownState::StoppingGuest) {
            dwarning("Safe toolbar shutdown timed out; syncing guest");
            startSafeShutdownSync();
        }
    });
    QObject::connect(this, &EmulatorQtWindow::requestUpdate, this,
                     &EmulatorQtWindow::slot_requestUpdate);
    QObject::connect(this, &EmulatorQtWindow::setDeviceGeometry, this,
                     &EmulatorQtWindow::slot_setDeviceGeometry);
    QObject::connect(this, &EmulatorQtWindow::setWindowIcon, this,
                     &EmulatorQtWindow::slot_setWindowIcon,
                     Qt::QueuedConnection);
    QObject::connect(this, &EmulatorQtWindow::setWindowPos, this,
                     &EmulatorQtWindow::slot_setWindowPos);
    QObject::connect(this, &EmulatorQtWindow::setWindowSize, this,
                     &EmulatorQtWindow::slot_setWindowSize);
    QObject::connect(this, &EmulatorQtWindow::paintWindowOverlayForResize, this,
                     &EmulatorQtWindow::slot_paintWindowOverlayForResize);
    QObject::connect(this, &EmulatorQtWindow::setWindowOverlayForResize, this,
                     &EmulatorQtWindow::slot_setWindowOverlayForResize);
    QObject::connect(this, &EmulatorQtWindow::clearWindowOverlay, this,
                     &EmulatorQtWindow::slot_clearWindowOverlay);
    QObject::connect(this, &EmulatorQtWindow::setWindowCursorResize, this,
                     &EmulatorQtWindow::slot_setWindowCursorResize);
    QObject::connect(this, &EmulatorQtWindow::setWindowCursorNormal, this,
                     &EmulatorQtWindow::slot_setWindowCursorNormal);
    QObject::connect(this, &EmulatorQtWindow::setTitle, this,
                     &EmulatorQtWindow::slot_setWindowTitle);
    QObject::connect(this, &EmulatorQtWindow::showWindow, this,
                     &EmulatorQtWindow::slot_showWindow);
    QObject::connect(this, &EmulatorQtWindow::updateRotation, this,
                     &EmulatorQtWindow::slot_updateRotation);
    QObject::connect(this, &EmulatorQtWindow::runOnUiThread, this,
                     &EmulatorQtWindow::slot_runOnUiThread);
    QObject::connect(this, &EmulatorQtWindow::showMessage, this,
                     &EmulatorQtWindow::slot_showMessage);
    QObject::connect(this, &EmulatorQtWindow::showMessageWithDismissCallback,
                     this,
                     &EmulatorQtWindow::slot_showMessageWithDismissCallback);
    QObject::connect(QApplication::instance(), &QCoreApplication::aboutToQuit,
                     this, &EmulatorQtWindow::slot_clearInstance);

    QObject::connect(mContainer.horizontalScrollBar(),
                     SIGNAL(valueChanged(int)), this,
                     SLOT(slot_horizontalScrollChanged(int)));
    QObject::connect(mContainer.verticalScrollBar(), SIGNAL(valueChanged(int)),
                     this, SLOT(slot_verticalScrollChanged(int)));
    QObject::connect(mContainer.horizontalScrollBar(),
                     SIGNAL(rangeChanged(int, int)), this,
                     SLOT(slot_scrollRangeChanged(int, int)));
    QObject::connect(mContainer.verticalScrollBar(),
                     SIGNAL(rangeChanged(int, int)), this,
                     SLOT(slot_scrollRangeChanged(int, int)));

    bool onTop = settings.value(Ui::Settings::ALWAYS_ON_TOP, false).toBool();
    setOnTop(onTop);

    // Our paintEvent() always fills the whole window.
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_AcceptTouchEvents);

    bool shortcutBool = false;
    if (!android::featurecontrol::isEnabled(
                android::featurecontrol::QtRawKeyboardInput)) {
        // If RawKeyboardInput feature is not enabled, read the settings value.
        // RawKeyboardInput feature always forwards the keyboard shortcuts to
        // the Device, which are not used by the emulator host itself.
        shortcutBool =
                settings.value(Ui::Settings::FORWARD_SHORTCUTS_TO_DEVICE, false)
                        .toBool();
    }
    setForwardShortcutsToDevice(shortcutBool ? 1 : 0);

    initErrorDialog(this, getConsoleAgents()
                                  ->settings->android_cmdLineOptions()
                                  ->qt_hide_window);
    setObjectName("MainWindow");
    mEventLogger->startRecording(this);
    mEventLogger->startRecording(mToolWindow);
    mEventLogger->startRecording(&mOverlay);
    mUserActionsCounter->startCountingForMainWindow(this);
    mUserActionsCounter->startCountingForToolWindow(mToolWindow);
    mUserActionsCounter->startCountingForOverlayWindow(&mOverlay);
    std::weak_ptr<android::qt::UserActionsCounter> userActionsWeak(
            mUserActionsCounter);
    using android::metrics::PeriodicReporter;
    mMetricsReportingToken = PeriodicReporter::get().addCancelableTask(
            60 * 10 * 1000,  // reporting period
            [userActionsWeak](android_studio::AndroidStudioEvent* event) {
                if (auto user_actions = userActionsWeak.lock()) {
                    const auto actionsCount = user_actions->count();
                    event->mutable_emulator_ui_event()->set_context(
                            android_studio::EmulatorUiEvent::
                                    UNKNOWN_EMULATOR_UI_EVENT_CONTEXT);
                    event->mutable_emulator_ui_event()->set_type(
                            android_studio::EmulatorUiEvent::
                                    UNKONWN_EMULATOR_UI_EVENT_TYPE);
                    event->mutable_emulator_ui_event()->set_value(actionsCount);
                    event->set_kind(android_studio::AndroidStudioEvent::
                                            EMULATOR_UI_EVENT);
                    return true;
                }
                return false;
            });

    setFrameAlways(mFrameAlways);

    mWheelScrollTimer.setInterval(15);
    connect(&mWheelScrollTimer, SIGNAL(timeout()), this,
            SLOT(wheelScrollTimeout()));

    mTrackpadAggregateTimer.setInterval(15);
    connect(&mTrackpadAggregateTimer, SIGNAL(timeout()), this,
            SLOT(trackpadAggregateTimeout()));

    mIgnoreWheelEvent =
            settings.value(Ui::Settings::DISABLE_MOUSE_WHEEL, false).toBool();

    if (avdInfo_isDesktopApi36OrHigher(
                getConsoleAgents()->settings->avdInfo()) &&
            !settings.contains(Ui::Settings::DISABLE_PINCH_TO_ZOOM)) {
        settings.setValue(Ui::Settings::DISABLE_PINCH_TO_ZOOM, true);
    }

    mDisablePinchToZoom =
            settings.value(Ui::Settings::DISABLE_PINCH_TO_ZOOM, false).toBool();
    // set custom ADB path if saved
    bool autoFindAdb =
            settings.value(Ui::Settings::AUTO_FIND_ADB, true).toBool();
    if (!autoFindAdb) {
        QString adbPath = settings.value(Ui::Settings::ADB_PATH, "").toString();
        if (!adbPath.isEmpty()) {
            adbPath = QDir::toNativeSeparators(adbPath);
            (*mAdbInterface)->setCustomAdbPath(adbPath.toStdString());
        }
    }

    mPauseAvdWhenMinimized = SettingsPage::getPauseAvdWhenMinimized();

    ScreenMask::loadMask();
    mScreenMaskPixmap = QPixmap::fromImage(ScreenMask::getMaskImage());
    mScreenMaskChanged = true;

    using android::snapshot::Snapshotter;
    Snapshotter::get().addOperationCallback([this](Snapshotter::Operation op,
                                                   Snapshotter::Stage stage) {
        if (stage == Snapshotter::Stage::Start) {
            runOnUiThread([this, op]() {
                AutoLock lock(mSnapshotStateLock);
                mShouldShowSnapshotModalOverlay = true;
                QTimer::singleShot(500, QApplication::instance(), [this, op]() {
                    AutoLock lock(mSnapshotStateLock);
                    if (mShouldShowSnapshotModalOverlay) {
                        mContainer.showModalOverlay(
                                op == Snapshotter::Operation::Save
                                        ? tr("Saving state...")
                                        : tr("Loading state..."));
                        if (op == Snapshotter::Operation::Save) {
                            mContainer.setModalOverlayFunc(tr("CANCEL"), [] {
                                androidSnapshot_cancelSave();
                            });
                        }
                    }
                });
                if (mToolWindow) {
                    mToolWindow->setEnabled(false);
                }
                if (mCarClusterWindow) {
                    mCarClusterWindow->setEnabled(false);
                }

                if (SnapshotPage::get()) {
                    SnapshotPage::get()->setOperationInProgress(true);
                }

            });
        } else if (stage == Snapshotter::Stage::End) {
            runOnUiThread([this, op]() {
                AutoLock lock(mSnapshotStateLock);
                mShouldShowSnapshotModalOverlay = false;
                mContainer.hideModalOverlay();
                if (mToolWindow) {
                    mToolWindow->setEnabled(true);
                }
                if (mCarClusterWindow) {
                    mCarClusterWindow->setEnabled(true);
                }
                if (SnapshotPage::get()) {
                    SnapshotPage::get()->setOperationInProgress(false);
                }

            });
        }
    });

    // Enable the close button on the tool window as the last step.
    mToolWindow->enableCloseButton();

    if (android::featurecontrol::isEnabled(
                android::featurecontrol::VirtioDualModeMouse)) {
        D("Dual-mode mouse: Setting abolute coordinates to true");
        // Enable absolute coordiantes mode as default for dual mode mouse.
        mRelativeMouseCoordMode = false;
        // Do not show the mouse restore prompt in dual-mode mouse driver since
        // the pointer will be grabbed and release seamlessly in relative
        // coordiantes mode without any other user actions.
        mPromptMouseRestoreMessageBox = false;
        // Enable mouse tracking from the initialization.
        queueSkinEvent(createMouseTrackingSkinEvent(kEventMouseStartTracking));
    }

    // Follow what the guest input method reports. The callback runs on the
    // thread that holds the VM lock; the destructor removes it before |this|
    // goes away. The cursor QRect carries the guest's left, top, right and
    // bottom as they are.
    mHostImeCallbackId = ModavdImePipe::addStateCallback(
            [this](const ModavdImePipe::State& state, unsigned changes) {
                const QRect cursor(QPoint(state.cursorLeft, state.cursorTop),
                                   QPoint(state.cursorRight, state.cursorBottom));
                emit runOnUiThread([this, connected = state.connected,
                                    active = state.active,
                                    inputType = state.inputType,
                                    hasCursor = state.hasCursor, cursor,
                                    changes]() {
                    onHostImeState(connected, active, inputType, hasCursor,
                                   cursor, changes);
                });
            });
}

EmulatorQtWindow::Ptr EmulatorQtWindow::getInstancePtr() {
    return sInstance.get();
}

EmulatorQtWindow* EmulatorQtWindow::getInstance() {
    return getInstancePtr().get();
}

EmulatorQtWindow::~EmulatorQtWindow() {
    {
        AutoLock lock(safeShutdownLock());
        if (sSafeShutdownWindow == this) {
            sSafeShutdownWindow = nullptr;
        }
    }
    ModavdImePipe::removeStateCallback(mHostImeCallbackId);
    if (mApkInstallCommand) {
        mApkInstallCommand->cancel();
    }
    mInstallDialog.clear();
    mPushDialog.clear();

    deleteErrorDialog();
    if (mToolWindow) {
        delete mToolWindow;
        mToolWindow = NULL;
    }

    if (mCarClusterWindow) {
        delete mCarClusterWindow;
        mCarClusterWindow = NULL;
    }

    if (mCarClusterConnector) {
        delete mCarClusterConnector;
        mCarClusterConnector = NULL;
    }

    AutoLock lock(mSnapshotStateLock);
    mShouldShowSnapshotModalOverlay = false;
    mContainer.hideModalOverlay();
    lock.unlock();

    stopThread();
}

void EmulatorQtWindow::focusOutEvent(QFocusEvent* event) {
    for (auto& key : mHeldModifiers) {
        SkinEvent skin_event = createSkinEvent(kEventKeyUp);
        skin_event.u.key.keycode = key;
        queueSkinEvent(std::move(skin_event));
    }
    mHeldModifiers.clear();
    // Host IME text mode: releases of held keys may now go elsewhere.
    mHostImeHeld.clear();
    QFrame::focusOutEvent(event);
}

void EmulatorQtWindow::showWin32DeprecationWarning() {
#ifdef _WIN32
    auto programBitness = System::get()->getProgramBitness();
    if (programBitness != 32) {
        return;
    }

    mWin32WarningBox->show();
#endif
}

void EmulatorQtWindow::showAvdArchWarning() {
    ScopedCPtr<char> arch(
            avdInfo_getTargetCpuArch(getConsoleAgents()->settings->avdInfo()));

    // On Apple, we could also be running w/ Virtualization.framework
    // which should also support fast x86 VMs on arm64.
#if defined(__APPLE__) || defined(__x86_64__)
    if (!strcmp(arch.get(), "x86") || !strcmp(arch.get(), "x86_64")) {
        return;
    }
#endif

#ifdef __aarch64__
    if (!strcmp(arch.get(), "arm64")) {
        return;
    }
#endif

    // The following statuses indicate that the machine hardware does not
    // support hardware acceleration. These machines should never show a
    // popup indicating to switch to x86.
    static const AndroidCpuAcceleration badStatuses[] = {
            ANDROID_CPU_ACCELERATION_NESTED_NOT_SUPPORTED,
            ANDROID_CPU_ACCELERATION_INTEL_REQUIRED,
            ANDROID_CPU_ACCELERATION_NO_CPU_SUPPORT,  // CPU doesn't support
                                                      // required
                                                      // features (VT-x or SVM)
            ANDROID_CPU_ACCELERATION_NO_CPU_VTX_SUPPORT,  // CPU doesn't support
                                                          // VT-x
            ANDROID_CPU_ACCELERATION_NO_CPU_NX_SUPPORT,   // CPU doesn't support
                                                          // NX
    };

    AndroidCpuAcceleration cpuStatus =
            androidCpuAcceleration_getStatus(nullptr);
    for (AndroidCpuAcceleration status : badStatuses) {
        if (cpuStatus == status) {
            return;
        }
    }

    QSettings settings;
    if (settings.value(Ui::Settings::SHOW_AVD_ARCH_WARNING, true).toBool()) {
        QObject::connect(mAvdWarningBox.ptr(),
                         SIGNAL(buttonClicked(QAbstractButton*)), this,
                         SLOT(slot_avdArchWarningMessageAccepted()));

        QCheckBox* checkbox = new QCheckBox(tr("Never show this again."));
        checkbox->setCheckState(Qt::Unchecked);
        mAvdWarningBox->setWindowModality(Qt::NonModal);
        mAvdWarningBox->setCheckBox(checkbox);
        mAvdWarningBox->show();
    }
}

void EmulatorQtWindow::checkShouldShowGpuWarning() {
    mShouldShowGpuWarning =
            globalGpuInfoList().blacklist_status &&
            (0 == strcmp(emuglConfig_get_user_gpu_option(), "") ||
             0 == strcmp(emuglConfig_get_user_gpu_option(), "auto") ||
             0 == strcmp(emuglConfig_get_user_gpu_option(), "auto-no-window"));
}

void EmulatorQtWindow::showGpuWarning() {
    if (!mShouldShowGpuWarning) {
        return;
    }

    QSettings settings;
    if (settings.value(Ui::Settings::SHOW_GPU_WARNING, true).toBool()) {
        QObject::connect(mGpuWarningBox.ptr(),
                         SIGNAL(buttonClicked(QAbstractButton*)), this,
                         SLOT(slot_gpuWarningMessageAccepted()));

        QCheckBox* checkbox = new QCheckBox(tr("Never show this again."));
        checkbox->setCheckState(Qt::Unchecked);
        mGpuWarningBox->setWindowModality(Qt::NonModal);
        mGpuWarningBox->setCheckBox(checkbox);
        mGpuWarningBox->show();
    }
}

void EmulatorQtWindow::slot_startupTick() {
    // this is useless as we are moving to embed already
    return;
}

void EmulatorQtWindow::slot_avdArchWarningMessageAccepted() {
    QCheckBox* checkbox = mAvdWarningBox->checkBox();
    if (checkbox->checkState() == Qt::Checked) {
        QSettings settings;
        settings.setValue(Ui::Settings::SHOW_AVD_ARCH_WARNING, false);
    }
}

void EmulatorQtWindow::slot_gpuWarningMessageAccepted() {
    QCheckBox* checkbox = mGpuWarningBox->checkBox();
    if (checkbox->checkState() == Qt::Checked) {
        QSettings settings;
        settings.setValue(Ui::Settings::SHOW_GPU_WARNING, false);
    }
}

void EmulatorQtWindow::closeEvent(QCloseEvent* event) {
    // make sure to use the close button on tool window
    if (!mToolWindow->isLeanToolbar() && isMainThreadRunning() &&
        !mToolWindow->closeButtonClicked()) {
        mToolWindow->on_close_button_clicked();
        event->ignore();
        return;
    }

    if (mToolWindow && isMainThreadRunning() &&
        mToolWindow->safeShutdownInProgress()) {
        mToolWindow->on_close_button_clicked();
        event->ignore();
        return;
    }

    if (!isMainThreadRunning() &&
        mSafeShutdownState != SafeShutdownState::Idle) {
        if (auto* modal = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            modal->close();
        }
        mSafeShutdownTimeout.stop();
        // The kill guard started with the shutdown still bounds the teardown.
        dinfo("Safe toolbar shutdown: the emulator has stopped; quitting");
        mSafeShutdownState = SafeShutdownState::Idle;
    }

    // make sure to use the close button on tool window
    if (!mToolWindow->shouldClose()) {
        event->ignore();
        return;
    }

    if (mToolWindow) {
        mToolWindow->setEnabled(false);
    }

    if (mCarClusterWindow) {
        mCarClusterWindow->setEnabled(false);
    }

    for (auto const& iter : mMultiDisplayWindow) {
        if (iter.second != nullptr)
            iter.second->setEnabled(false);
    }

    saveMultidisplayToConfig();

    const bool alreadyClosed = mClosed;
    mClosed = true;
    sClosed = true;
    crashhandler_exitmode(__FUNCTION__);

    // Make sure we cancel everything related to startup dialog here, otherwise
    // it could remain as the only emulator's window and would keep it running
    // forever.
    mStartupTimer.stop();
    mStartupTimer.disconnect();

    if (mMainLoopThread && mMainLoopThread->isRunning()) {
        if (!alreadyClosed) {
            // we dont want to restore to a state where the
            // framework is shut down by 'adb reboot -p'
            // so skip that step when saving vm on exit
            const bool fastSnapshotV1 =
                    android::featurecontrol::isEnabled(
                            android::featurecontrol::FastSnapshotV1);
            if (fastSnapshotV1) {
                // Tell the system that we are in saving; create a file lock.
                auto snapshotLockFilePath = avdInfo_getSnapshotLockFilePath(
                        getConsoleAgents()->settings->avdInfo());
                if (snapshotLockFilePath &&
                    !filelock_create(snapshotLockFilePath)) {
                    derror("unable to lock snapshot save on exit!\n");
                }
            }

            if (fastSnapshotV1) {
                queueQuitEvent();
            } else {
                runAdbShellPowerDownAndQuit();
            }
        }
        event->ignore();
    } else {
        mContainer.hideModalOverlay();

        if (mToolWindow) {
            mToolWindow->hide();
            mToolWindow->closeExtendedWindow();
        }
        if (mCarClusterWindow) {
            mCarClusterWindow->hide();
        }
        for (auto const& iter : mMultiDisplayWindow) {
            if (iter.second != nullptr)
                iter.second->hide();
        }
        mContainer.hide();
        mOverlay.hide();
        hide();
        event->accept();
        if (mHiddenForSafeShutdown) {
            // The windows were hidden when the safe shutdown started, so this
            // close is not the last visible window closing and Qt would not
            // leave its event loop: the process would end only by the kill
            // guard, without the normal teardown. Quit here, as the hidden
            // window mode does in EmulatorContainer::closeEvent.
            qApp->quit();
        }
    }
}

void EmulatorQtWindow::queueQuitEvent() {
    queueSkinEvent(createSkinEvent(kEventQuit));
}

void EmulatorQtWindow::dragEnterEvent(QDragEnterEvent* event) {
    // Accept all drag enter events with any URL, then filter more in drop
    // events
    if (event->mimeData() && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void EmulatorQtWindow::dragLeaveEvent(QDragLeaveEvent* event) {
    QToolTip::hideText();
}

void EmulatorQtWindow::dragMoveEvent(QDragMoveEvent* event) {
    QToolTip::showText(mapToGlobal(event->pos()),
                       tr("APK's will be installed.<br/>"
                          "Other files will be copied to /sdcard/Download."));
}

void EmulatorQtWindow::dropEvent(QDropEvent* event) {
    // Modal dialogs don't prevent drag-and-drop! Manually check for a modal
    // dialog, and if so, reject the event.
    if (QApplication::activeModalWidget() != nullptr) {
        event->ignore();
        return;
    }

    QList<QUrl> urls = event->mimeData()->urls();
    if (urls.length() == 0) {
        showErrorDialog(
                tr("Dropped content didn't have any files. Emulator only "
                   "supports drag-and-drop for files to copy them or install "
                   "a single APK."),
                tr("Drag and Drop"));
        return;
    }

    // Get the first url - if it's an APK and the only file, attempt to install
    // it
    QString url = urls[0].toLocalFile();
    if (url.endsWith(".apk") && urls.length() == 1) {
        runAdbInstall(url);
        return;
    } else {
        // If any of the files is an APK, intent was ambiguous
        for (int i = 0; i < urls.length(); i++) {
            if (urls[i].path().endsWith(".apk")) {
                showErrorDialog(
                        tr("Drag-and-drop can either install a single APK"
                           " file or copy one or more non-APK files to the"
                           " Emulator SD card."),
                        tr("Drag and Drop"));
                return;
            }
        }
        runAdbPush(urls);
    }
}

void EmulatorQtWindow::keyPressEvent(QKeyEvent* event) {
    if (hostImeKeyPress(*event)) {
        return;
    }
    const bool hostImeReleaseNow = mHostImeReleaseAfterPress;
    mHostImeReleaseAfterPress = false;

    if (android::featurecontrol::isEnabled(
                android::featurecontrol::QtRawKeyboardInput) &&
        event->isAutoRepeat()) {
        event->ignore();
        return;
    }

    handleKeyEvent(kEventKeyDown, *event);
    if (hostImeReleaseNow) {
        hostImeReleaseRawKey({HostImePath::Raw, event->key(),
                              event->nativeScanCode(),
                              event->nativeVirtualKey()});
    }
}

void EmulatorQtWindow::keyReleaseEvent(QKeyEvent* event) {
    if (hostImeKeyRelease(*event)) {
        return;
    }

    if (android::featurecontrol::isEnabled(
                android::featurecontrol::QtRawKeyboardInput) &&
        event->isAutoRepeat()) {
        event->ignore();
        return;
    }

    if (mOverlay.isVisible() && event->key() == Qt::Key_Control) {
        mOverlay.keyReleaseEvent(event);
        return;
    }

    handleKeyEvent(kEventKeyUp, *event);

    // If we enabled trackball mode, tell Qt to always forward mouse movement
    // events. Otherwise, Qt will forward them only when a button is pressed.
    EmulatorWindow* ew = emulator_window_get();
    bool trackballActive = skin_ui_is_trackball_active(ew->ui);
    if (trackballActive != hasMouseTracking()) {
        setMouseTracking(trackballActive);
    }
}
void EmulatorQtWindow::setRelativeMouseCoordMode(bool state) {
    D("Dual-mode Mouse: Setting mouse coordinates to %d", state);
    mRelativeMouseCoordMode = state;
}

const char* EmulatorQtWindow::releaseMouseShortcutName() const {
    if (avdInfo_isDesktopApi36OrHigher(
                getConsoleAgents()->settings->avdInfo())) {
#ifdef __APPLE__
        return "Cmd+Option+7";
#else
        return "Ctrl+Alt+7";
#endif
    }
#ifdef __APPLE__
    return "Cmd+R";
#else
    return "Ctrl+R";
#endif
}

bool EmulatorQtWindow::isReleaseMouseShortcut(SkinEventType type,
                                              const QKeyEvent& event) const {
    if (avdInfo_isDesktopApi36OrHigher(
                getConsoleAgents()->settings->avdInfo())) {
        return event.key() == Qt::Key_7 &&
            event.modifiers() == (Qt::ControlModifier | Qt::AltModifier) &&
            type == kEventKeyDown;
    }
    return event.key() == Qt::Key_R &&
        event.modifiers() == Qt::ControlModifier && type == kEventKeyDown;
}

void EmulatorQtWindow::updateWindowTitle() {
    if (mBaseWindowTitle.isEmpty()) {
        mBaseWindowTitle = mContainer.windowTitle();
    }
    if (mMouseGrabbed) {
        QString title = QString("[Mouse captured, press %1 to release] %2")
            .arg(releaseMouseShortcutName())
            .arg(mBaseWindowTitle);
        if (mSafeShutdownState != SafeShutdownState::Idle) {
            title += QString::fromUtf8("（正在关机…）");
        }
        mContainer.setWindowTitle(title);
    } else {
        mContainer.setWindowTitle(mBaseWindowTitle +
                                  (mSafeShutdownState != SafeShutdownState::Idle
                                           ? QString::fromUtf8("（正在关机…）")
                                           : QString()));
    }
}

#ifdef __APPLE__

void EmulatorQtWindow::grabMouse(const QCursor& cursor) {
    QFrame::grabMouse(cursor);
    // b/476267783: On Mac, setting a blank cursor does not necessarily hide
    // the cursor. We also invoke the native API to hide the cursor.
    if (cursor == Qt::BlankCursor) {
        mac_hide_cursor();
        mIsCursorHidden = true;
    }
}

void EmulatorQtWindow::releaseMouse() {
    QFrame::releaseMouse();
    if (mIsCursorHidden) {
        mac_show_cursor();
        mIsCursorHidden = false;
    }
}

#endif  // __APPLE__

void EmulatorQtWindow::mouseMoveEvent(QMouseEvent* event) {
    // The motion event will interfere with the swipe gesture being synthesized.
    if (mWheelScrollTimer.isActive())
        return;

    if (android::featurecontrol::isEnabled(
                android::featurecontrol::VirtioMouse) &&
        !getConsoleAgents()
                 ->settings->android_cmdLineOptions()
                 ->no_mouse_reposition) {
        // Block all the incoming mouse events if mouse is being moved to
        // center of the screen.
        if (!mMouseRepositioning &&
            (event->source() == Qt::MouseEventNotSynthesized)) {
            // Pen long press generates synthesized mouse events,
            // which need to be filtered out
            SkinEventType eventType = translateMouseEventType(
                    kEventMouseMotion, event->button(), event->buttons());
            handleMouseEvent(eventType, getSkinMouseButton(event), event->pos(),
                             event->globalPos(), /*skipSync*/ false,
                             mRelativeMouseCoordMode);
        }

        if (mMouseGrabbed) {
            QRect mouseRect(mContainer.rect().center() -
                                    QPoint(mContainer.rect().width() * 0.35f,
                                           mContainer.rect().height() * 0.35f),
                            mContainer.rect().center() +
                                    QPoint(mContainer.rect().width() * 0.35f,
                                           mContainer.rect().height() * 0.35f));
            // There may be multiple events to be handled when the mouse is
            // repositioned to the center of the window, thus we block all the
            // incoming mouseMoveEvents until the mouse position is completely
            // within the |mouseRect|.
            if (!mouseRect.contains(event->pos())) {
                if (!mMouseRepositioning) {
                    mMouseRepositioning = true;
                    QCursor::setPos(
                            mContainer.mapToGlobal(mContainer.rect().center()));
                    mPrevMousePosition = mContainer.rect().center();
                }
            } else {
                mMouseRepositioning = false;
            }
        }
    } else {
        // Pen long press generates synthesized mouse events,
        // which need to be filtered out
        if (event->source() == Qt::MouseEventNotSynthesized) {
            SkinEventType eventType = translateMouseEventType(
                    kEventMouseMotion, event->button(), event->buttons());
            handleMouseEvent(eventType, getSkinMouseButton(event), event->pos(),
                             event->globalPos());
        }
    }
}

Qt::CursorShape EmulatorQtWindow::getCursorShape(bool mouseGrabbed) {
    if (android::featurecontrol::isEnabled(
                android::featurecontrol::VirtioDualModeMouse)) {
        if (mRelativeMouseCoordMode) {
            return DUAL_MODE_MOUSE_RELATIVE_MODE_CURSOR;
        } else {
            if (android::featurecontrol::isEnabled(
                        android::featurecontrol::
                                DualModeMouseDisplayHostCursor)) {
                return DUAL_MODE_MOUSE_DEFAULT_CURSOR_DISPLAY_HOST_MODE;
            } else {
                return DUAL_MODE_MOUSE_DEFAULT_CURSOR_DISPLAY_GUEST_MODE;
            }
        }
    } else {
        if (mouseGrabbed) {
            return DEFAULT_MOUSE_GRABBED_MODE_CURSOR;
        } else {
            return DEFAULT_MOUSE_NORMAL_MODE_CURSOR;
        }
    }
}

void EmulatorQtWindow::enterEvent(QEnterEvent* event) {
    if (android::featurecontrol::isEnabled(
                android::featurecontrol::VirtioDualModeMouse)) {
        // If dual-mode driver is enabled, change the mouse cursor shape when
        // cursor enters the window area.
        mContainer.setCursor(getCursorShape(/*mouseGrabbed*/ false));
    }
}

void EmulatorQtWindow::leaveEvent(QEvent* event) {
    // On Mac, the cursor retains its shape even after it
    // leaves, if it is over our transparent region. Make
    // sure we're not showing the resize cursor. We can't
    // resize because we don't get events after the cursor
    // has left.
    // In case of VirtioDualModeMouse, restores the cursor
    // to host cursor.
    mContainer.setCursor(Qt::ArrowCursor);
}

void EmulatorQtWindow::mousePressEvent(QMouseEvent* event) {
    // Enable the mouse grab for VirtioMouse. If VirtioDualModeMouse is
    // enabled, mouse grab is enabled only in the relative coordinates mode.
    // TODO(b/368630314): On mac os, the cursor shape does not change on button
    // press and the bounding rect used to contain the relative cursor causes
    // visual artifacts. This may require additional debugging and a possible
    // QT changes. Hence, grabbed mode is disabled on VirtioDualModeMouse for
    // now.
    bool enableMouseGrab =
            android::featurecontrol::isEnabled(
                    android::featurecontrol::VirtioMouse) &&
            !android::featurecontrol::isEnabled(
                    android::featurecontrol::VirtioDualModeMouse);
    if (enableMouseGrab && !mMouseGrabbed) {
        if (mPromptMouseRestoreMessageBox) {
            QMessageBox msgBox;
            QString text = QString(
                    "You have clicked the mouse inside the Emulator "
                    "display. This will cause Emulator to capture the "
                    "host mouse pointer and the keyboard, which will "
                    "make them unavailable to other host applications."
                    "<br><br>You can press %1 to uncapture the keyboard and "
                    "mouse and return to normal operation."
                    ).arg(releaseMouseShortcutName());

            if (!getConsoleAgents()
                         ->settings->android_cmdLineOptions()
                         ->no_mouse_reposition) {
                text.append(
                        "<br><br>On remote desktops, mouse might not work "
                        "properly. Please enable relative mouse mode on "
                        "your remote desktop software, or add "
                        "\"--no-mouse-reposition\" argument to your "
                        "emulator arguments.");
            }
            msgBox.setText(text);
            msgBox.setTextFormat(Qt::TextFormat::AutoText);
            msgBox.exec();
            mPromptMouseRestoreMessageBox = false;
        }

        D("%s: mouse grabbed\n", __func__);
        grabMouse(QCursor(getCursorShape(/*mouseGrabbed*/ true)));

        queueSkinEvent(createMouseTrackingSkinEvent(kEventMouseStartTracking));

        mMouseGrabbed = true;
        updateWindowTitle();
        if (!android::featurecontrol::isEnabled(
                    android::featurecontrol::VirtioDualModeMouse)) {
            // Don't send this mouse event to the guest. Either the user likely
            // clicked on the dialog that popped up (rendering the press/release
            // state incorrect), or they just clicked in the window to activate
            // mouse input and should retain control over the guest OS behavior.
            //
            // NOTE: This will potentially cause a spurrious mouse release event
            // to be sent when the user releases the button, but this is
            // considered unlikely to affect things, and not worth the risk of
            // tracking it.
            return;
        }
    }

    // Pen long press generates synthesized mouse events,
    // which need to be filtered out
    if (event->source() == Qt::MouseEventNotSynthesized) {
        SkinEventType eventType = translateMouseEventType(
                kEventMouseButtonDown, event->button(), event->buttons());
        handleMouseEvent(eventType, getSkinMouseButton(event), event->pos(),
                         event->globalPos());
    }
}

void EmulatorQtWindow::mouseReleaseEvent(QMouseEvent* event) {
    // Pen long press generates synthesized mouse events,
    // which need to be filtered out
    if (android::featurecontrol::isEnabled(
                android::featurecontrol::VirtioDualModeMouse) &&
        mRelativeMouseCoordMode && mMouseGrabbed) {
        D("Mouse Released");
        releaseMouse();
        mMouseGrabbed = false;
        updateWindowTitle();
        mContainer.setCursor(getCursorShape(/*mouseGrabbed*/ false));
    }
    if (event->source() == Qt::MouseEventNotSynthesized) {
        SkinEventType eventType = translateMouseEventType(
                kEventMouseButtonUp, event->button(), event->buttons());
        handleMouseEvent(eventType, getSkinMouseButton(event), event->pos(),
                         event->globalPos());
    }
}

void EmulatorQtWindow::focusInEvent(QFocusEvent* event) {
    // FocusIn event may not be received unless setFocusPolicy(Qt::StrongFocus)
    // is called on the widget. However, this breaks the zoom behavior because
    // of EmulatorOverlay::focusOutEvent() hides the overlay itself right after
    // the zoom button is clicked since it takes the focus out of the emulator
    // window.
    if (android::featurecontrol::isEnabled(
                android::featurecontrol::VirtioDualModeMouse)) {
        mContainer.setCursor(getCursorShape(/*mouseGrabbed*/ false));
    }
}

// Event handler for pen events as defined by Qt
void EmulatorQtWindow::tabletEvent(QTabletEvent* event) {
    SkinEventType eventType = kEventPenRelease;
    bool button_pressed = false;

    switch (event->type()) {
        case QEvent::TabletPress: {
            button_pressed =
                    ((event->buttons() & Qt::RightButton) == Qt::RightButton);
            eventType = translatePenEventType(kEventPenPress, event->button(),
                                              event->buttons());
            handlePenEvent(eventType, event);
            mPenPosX = event->pos().x();
            mPenPosY = event->pos().y();
            mPenButtonPressed = button_pressed;
            break;
        }
        case QEvent::TabletRelease: {
            button_pressed =
                    ((event->buttons() & Qt::RightButton) == Qt::RightButton);
            eventType = translatePenEventType(kEventPenRelease, event->button(),
                                              event->buttons());
            handlePenEvent(eventType, event);
            mPenPosX = event->pos().x();
            mPenPosY = event->pos().y();
            mPenButtonPressed = button_pressed;
            break;
        }
        case QEvent::TabletMove: {
            button_pressed =
                    ((event->buttons() & Qt::RightButton) == Qt::RightButton);
            if ((event->pos().x() != mPenPosX) ||
                (event->pos().y() != mPenPosY) ||
                (button_pressed != mPenButtonPressed)) {
                eventType = translatePenEventType(
                        kEventPenMove, event->button(), event->buttons());
                handlePenEvent(eventType, event);
                mPenPosX = event->pos().x();
                mPenPosY = event->pos().y();
                mPenButtonPressed = button_pressed;
            }
            break;
        }
        default:
            break;
    }
    event->accept();
}

// Set the window flags based on whether we should
// have a frame or not.
// Mask off the background of the skin PNG if we
// are running frameless.

void EmulatorQtWindow::maskWindowFrame() {
    bool haveFrame = hasFrame();
    Qt::WindowFlags flags = mContainer.windowFlags();

    flags &= ~FRAME_WINDOW_FLAGS_MASK;
    flags |= (haveFrame ? FRAMED_WINDOW_FLAGS : FRAMELESS_WINDOW_FLAGS);
#ifdef _WIN32
    if (ToolWindow::inWindowNav() && haveFrame) {
        flags |= Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint |
                 Qt::WindowCloseButtonHint;
        flags &= ~Qt::WindowMaximizeButtonHint;
    }
#endif

    if (!ToolWindow::inWindowNav() || mContainer.windowFlags() != flags) {
        mContainer.setWindowFlags(flags);
    }

    // Re-generate and apply the mask
    if (haveFrame) {
        // We have a frame. Do not use a mask around the device.
#ifdef _WIN32
        mContainer.clearMask();
#else
        // On Linux and Mac, clearMask() doesn't seem to work,
        // so we create a full rectangular mask. (Which doesn't
        // work right on Windows!)
        QRegion fullRegion(0, 0, mContainer.width(), mContainer.height());
        mContainer.setMask(fullRegion);

        if (!mHaveBeenFrameless) {
            // This is necessary on Mac. Doesn't hurt on Linux.
            mContainer.clearMask();
        }
#endif
        // We have a frame. There is no transparent gap around the skin.
        mSkinGapTop = 0;
        mSkinGapRight = 0;
        mSkinGapBottom = 0;
        mSkinGapLeft = 0;
    } else {
        // Frameless: Do an intelligent mask.
        // Start by reloading the skin PNG file.
        mHaveBeenFrameless = true;

        if (EmulatorSkin::getInstance()->getSkinPixmap() != nullptr) {
            // Rotate the skin to match the emulator window
            QTransform rotater;
            // Set the native rotation of the skin image

            int rotationAmount = 0;
            // Adjust for user-initiated rotation
            switch (mOrientation) {
                case SKIN_ROTATION_0:
                    rotationAmount += 0;
                    break;
                case SKIN_ROTATION_90:
                    rotationAmount += 90;
                    break;
                case SKIN_ROTATION_180:
                    rotationAmount += 180;
                    break;
                case SKIN_ROTATION_270:
                    rotationAmount += 270;
                    break;
                default:
                    rotationAmount += 0;
                    break;
            }
            if (rotationAmount >= 360)
                rotationAmount -= 360;
            rotater.rotate(rotationAmount);
            QPixmap rotatedPMap(
                    EmulatorSkin::getInstance()->getSkinPixmap()->transformed(
                            rotater));

            // Scale the bitmap to the current window size
            int width = mContainer.width();
            QPixmap scaledPixmap = rotatedPMap.scaledToWidth(width);

            // Convert from bit map to a mask
            QBitmap bitmap = scaledPixmap.mask();
            setVisibleExtent(bitmap);

            // Determine if this is a round device.
            // If this mask is circular, it will be transparent near
            // the corners of the bounding rectangle. The top left of
            // the circle will be at (1 - 1/sqrt(2))*diameter = 0.146*w.
            QImage mapImage = bitmap.toImage();
            int height = mContainer.height();
            int opaqueWidth = width - mSkinGapLeft - mSkinGapRight;
            int opaqueHeight = height - mSkinGapTop - mSkinGapBottom;
            int xTest = mSkinGapLeft + 0.146 * opaqueWidth - 4;
            int yTest = mSkinGapTop + 0.146 * opaqueHeight - 4;
            if (mapImage.size().width() > xTest) {
                // The map is not null. It is round if the test pixel is ones.
                // (If it is null, it's not doing anything, which is
                // rectangular.)
                mBackingSurface->isRound =
                        (mapImage.pixel(xTest, yTest) == 0xFFFFFFFF);
            }
#ifdef __APPLE__
            // On Mac, the mask is automatically stretched so its
            // rectangular extent is as big as the widget it is
            // applied to. To avoid stretching, set two points to
            // make the mask's extent the full widget size.
            QPainter painter(&bitmap);
            painter.setBrush(Qt::black);
            QPoint twoPoints[2] = {
                    QPoint(bitmap.width() - 1, 0),    // North east
                    QPoint(0, bitmap.height() - 1)};  // South west
            painter.drawPoints(twoPoints, 2);
#endif
            // Apply the mask
            mContainer.setMask(bitmap);
        }
    }
    if (!mHiddenForSafeShutdown) {
        mContainer.show();
    }
    mToolWindow->dockMainWindow();

    if (haveFrame != mPreviouslyFramed) {
        // We are switching between framed and frameless. We need to
        // signal a kEventScreenChanged to force redrawing the device
        // screen.
        // Unfortunately, signaling this once isn't sufficient; we
        // need to do it until things settle down. Three time looks
        // like enough. Five times is for safety, but even this is
        // not a 100% fix.
        // TODO: http://b/78789992 Occasional black screen when switching
        // framed/frameless
        mHardRefreshCountDown = 5;
        mPreviouslyFramed = haveFrame;
    }
    SkinEventType eventType;
    if (mHardRefreshCountDown == 0) {
        D("%s: kEventWindowChanged", __FUNCTION__);
        eventType = kEventWindowChanged;
    } else {
        D("%s: kEventScreenChanged", __FUNCTION__);
        eventType = kEventScreenChanged;
        mHardRefreshCountDown--;
    }
    if (eventType == kEventScreenChanged) {
        queueSkinEvent(createSkinEventScreenChanged());
    } else {
        queueSkinEvent(createSkinEvent(eventType));
    }
}

void EmulatorQtWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    QRect bg(QPoint(0, 0), this->size());
    painter.fillRect(bg, Qt::black);

    if (mBackingSurface) {
        double dpr = 1.0;
        slot_getDevicePixelRatio(&dpr);
        QRect r(0, 0, mBackingSurface->w, mBackingSurface->h);
        if (mBackingBitmapChanged) {
            mScaledBackingImage =
                    QPixmap::fromImage(mBackingSurface->bitmap->get().scaled(
                            r.size() * dpr, Qt::KeepAspectRatio,
                            Qt::SmoothTransformation));
            mBackingBitmapChanged = false;
        }
        if (!mScaledBackingImage.isNull()) {
            if (mScaledBackingImage.devicePixelRatio() != dpr) {
                mScaledBackingImage.setDevicePixelRatio(dpr);
            }
            painter.drawPixmap(r, mScaledBackingImage);
        }
    }
    if (!mGuestScreenPixmap.isNull()) {
        // Draw the pixmap to fill the frame's contents rect or device geometry if available
        auto r = (mBackingSurface && mDeviceGeometry.isValid()) ? mDeviceGeometry : contentsRect();
        painter.drawPixmap(r, mGuestScreenPixmap);
        if (mScreenMaskChanged) {
            updateScreenMask();
        }
        if (!mRotatedScreenMaskPixmap.isNull()) {
            painter.drawPixmap(r, mRotatedScreenMaskPixmap);
        }
    }
}

void EmulatorQtWindow::raise() {
    mContainer.raise();
    mToolWindow->raise();
    if (mCarClusterWindow) {
        mCarClusterWindow->raise();
    }
}

void EmulatorQtWindow::setSharedMemoryRenderer(SharedMemoryRenderer* renderer) {
    mSharedMemoryRenderer.reset(renderer);
    if (mSharedMemoryRenderer) {
        // Connect the signal from the renderer to our slot.
        // Qt will handle the cross-thread invocation automatically.
        connect(mSharedMemoryRenderer.get(), &SharedMemoryRenderer::frameReady,
                this, &EmulatorQtWindow::slot_updateGuestScreen);
    }
}

void EmulatorQtWindow::initializeStreamer(std::string_view shm_handle,
                                          StreamTransport transport_type) {
    mTransportType = transport_type;
    using android::emulation::control::Image;
    using android::emulation::control::ImageFormat;

    int width = getConsoleAgents()->settings->hw()->hw_lcd_width;
    int height = getConsoleAgents()->settings->hw()->hw_lcd_height;

    connect(this, &EmulatorQtWindow::frameReady, this,
            &EmulatorQtWindow::slot_updateGuestScreen);

    if (transport_type == StreamTransport::MMAP && !shm_handle.empty()) {
        mSharedMemoryRenderer = std::make_unique<SharedMemoryRenderer>(
                std::string(shm_handle), width, height,
                SharedMemoryRenderer::ImgFormat::RGB888);
        mSharedMemoryRenderer->initialize();

        connect(mSharedMemoryRenderer.get(), &SharedMemoryRenderer::frameReady,
                this, &EmulatorQtWindow::slot_updateGuestScreen);
    }

    mStreamer = std::make_unique<SharedStreamEmulator>(
            shm_handle,
            [this, transport_type](const Image* image) {
                if (image && image->has_format()) {
                    mActiveDisplayId = image->format().display();
                }
                if (transport_type == StreamTransport::MMAP) {
                    if (mSharedMemoryRenderer) {
                        mSharedMemoryRenderer->update();
                    }
                } else {
                    const std::string& data = image->image();
                    if (data.empty()) {
                        return;
                    }

                    QImage qimage;
                    switch (image->format().format()) {
                        case ImageFormat::PNG:
                            qimage.loadFromData(
                                    reinterpret_cast<const uchar*>(data.data()),
                                    static_cast<int>(data.size()), "PNG");
                            break;
                        case ImageFormat::RGBA8888:
                            qimage = QImage(
                                    reinterpret_cast<const uchar*>(data.data()),
                                    image->format().width(),
                                    image->format().height(),
                                    QImage::Format_RGBA8888);
                            qimage = qimage.copy();
                            break;
                        case ImageFormat::RGB888:
                            qimage = QImage(
                                    reinterpret_cast<const uchar*>(data.data()),
                                    image->format().width(),
                                    image->format().height(),
                                    QImage::Format_RGB888);
                            qimage = qimage.copy();
                            break;
                        default:
                            return;
                    }
                    if (!qimage.isNull()) {
                        emit frameReady(qimage);
                    }
                }
            },
            // Shared memory (MMAP) transport requires pre-allocated fixed buffer dimensions.
            // Standard gRPC streaming adapts dynamically to incoming image frame packet sizes,
            // so we initialize dimensions to 0 to allow dynamic adaptation.
            (transport_type == StreamTransport::MMAP) ? width : 0,
            (transport_type == StreamTransport::MMAP) ? height : 0,
            transport_type);
    mStreamer->startStream();
}

void EmulatorQtWindow::slot_updateGuestScreen(const QImage& frame) {
    // This code runs safely on the UI thread.
    mGuestScreenPixmap = QPixmap::fromImage(frame);
    // Trigger a repaint of the window to display the new pixmap.
    update();
}

void EmulatorQtWindow::show() {
    if (mClosed || mHiddenForSafeShutdown) {
        return;
    }
    mContainer.show();
    QFrame::show();
    mToolWindow->show();
    if (mCarClusterWindow) {
        mCarClusterConnector->start();
    }

    QObject::connect(window()->windowHandle(), &QWindow::screenChanged, this,
                     &EmulatorQtWindow::onScreenChanged);
    QObject::connect(qGuiApp, &QGuiApplication::primaryScreenChanged, this,
                     &EmulatorQtWindow::onScreenConfigChanged);
    QObject::connect(qGuiApp, &QGuiApplication::screenAdded, this,
                     &EmulatorQtWindow::onScreenConfigChanged);
    QObject::connect(qGuiApp, &QGuiApplication::screenRemoved, this,
                     &EmulatorQtWindow::onScreenConfigChanged);
}

void EmulatorQtWindow::setOnTop(bool onTop) {
    setFrameOnTop(&mContainer, onTop);
    setFrameOnTop(mToolWindow, onTop);
    if (mCarClusterWindow) {
        setFrameOnTop(mCarClusterWindow, onTop);
    }
}

void EmulatorQtWindow::setFrameAlways(bool frameAlways) {
    mFrameAlways = frameAlways;
    if (mStartupDone) {
        maskWindowFrame();
        mContainer.show();
    }

    D("%s: kEventScreenChanged", __FUNCTION__);
    queueSkinEvent(createSkinEventScreenChanged());
}

void EmulatorQtWindow::setIgnoreWheelEvent(bool ignore) {
    mIgnoreWheelEvent = ignore;
}

void EmulatorQtWindow::setDisablePinchToZoom(bool disabled) {
    mDisablePinchToZoom = disabled;
}

void EmulatorQtWindow::setPauseAvdWhenMinimized(bool pause) {
    mPauseAvdWhenMinimized = pause;
}

void EmulatorQtWindow::showMinimized() {
#ifndef _WIN32
    // For Mac and Linux, ensure that the window has a frame and
    // minimize buttons while it is minimized.
    // Why?
    //   -  Mac won't un-minimize unless those exist.
    //   -  Linux adds those in automatically on minimize. This
    //      code path gets us to remove them on un-minimize.

    Qt::WindowFlags flags = mContainer.windowFlags();
#ifdef __linux__
//    b/282895205, temporarily remove the follow hacks
//    as newer linux desktop behave differently
//    flags &= ~FRAME_WINDOW_FLAGS_MASK;
//    flags |= FRAMED_WINDOW_FLAGS;
#else   // __APPLE__
    if (hasFrame()) {
        flags |= Qt::NoDropShadowWindowHint;
    } else {
        // Ignore the previous flags
        flags = Qt::Window | Qt::WindowMinMaxButtonsHint;
    }
#endif  // __APPLE__
    mContainer.setWindowFlags(flags);

#endif  // !_WIN32
    mContainer.showMinimized();

#ifdef __linux__
    // need to call toowindow hide again
    mToolWindow->hide();
#endif  // __linux__

    mWindowIsMinimized = true;

    if (mPauseAvdWhenMinimized) {
        (*mAdbInterface)
                ->runAdbCommand(
                        {"emu", "avd", "pause"},
                        [this](const android::emulation::
                                       OptionalAdbCommandResult&) { ; },
                        5000);
    }
}

void EmulatorQtWindow::handleTouchPoints(const QTouchEvent& touch,
                                         uint32_t displayId) {
    unsigned char nrOfPoints = 0;
    unsigned char lastValidTouchPointIndex = 0;
    for (int i = touch.touchPoints().count() - 1; i >= 0; --i) {
        if (touch.touchPoints()[i].state() != QEventPoint::Stationary) {
            lastValidTouchPointIndex = i;
            break;
        }
    }
    for (auto const& elem : touch.touchPoints()) {
        SkinEvent skin_event;
        bool eventSet = false;
        if (elem.state() == QEventPoint::Pressed) {
            skin_event = createSkinEvent(kEventTouchBegin);
            eventSet = true;
        } else if (elem.state() == QEventPoint::Updated) {
            skin_event = createSkinEvent(kEventTouchUpdate);
            eventSet = true;
        } else if (elem.state() == QEventPoint::Released) {
            skin_event = createSkinEvent(kEventTouchEnd);
            eventSet = true;
        }
        if (eventSet) {
            bool lasElementFound = false;
            skin_event.u.multi_touch_point.x = elem.pos().x();
            skin_event.u.multi_touch_point.y = elem.pos().y();
            skin_event.u.multi_touch_point.x_global = elem.screenPos().x();
            skin_event.u.multi_touch_point.y_global = elem.screenPos().y();
            skin_event.u.multi_touch_point.id = elem.id();
            skin_event.u.multi_touch_point.pressure =
                    (int)(elem.pressure() * MTS_PRESSURE_RANGE_MAX);
            skin_event.u.multi_touch_point.touch_minor =
                    std::min(elem.ellipseDiameters().rheight(),
                             elem.ellipseDiameters().rwidth());
            skin_event.u.multi_touch_point.touch_major =
                    std::max(elem.ellipseDiameters().rheight(),
                             elem.ellipseDiameters().rwidth());
            skin_event.u.multi_touch_point.orientation = elem.rotation();
            if (nrOfPoints == lastValidTouchPointIndex) {
                skin_event.u.multi_touch_point.skip_sync = false;
                lasElementFound = true;
            } else {
                skin_event.u.multi_touch_point.skip_sync = true;
            }
            skin_event.u.multi_touch_point.display_id = displayId;

            queueSkinEvent(std::move(skin_event));
            if (lasElementFound) {
                break;
            }
        }
        ++nrOfPoints;
    }
}

bool EmulatorQtWindow::event(QEvent* ev) {
    if (ev->type() == QEvent::TouchBegin || ev->type() == QEvent::TouchUpdate ||
        ev->type() == QEvent::TouchEnd) {
        QTouchEvent* touchEvent = static_cast<QTouchEvent*>(ev);
        // b/216383452 Only process multitouch events generated from touch
        // screen.
        if (touchEvent->device()->type() ==
            QInputDevice::DeviceType::TouchScreen) {
            handleTouchPoints(*touchEvent);
            return true;
        }
    }
    if (ev->type() == QEvent::WindowActivate && mWindowIsMinimized) {
        mWindowIsMinimized = false;
        // note: this is intentional, as user could pause the avd throu console
        // and we want to make the avd usable.
        if (true) {
            (*mAdbInterface)
                    ->runAdbCommand(
                            {"emu", "avd", "resume"},
                            [this](const android::emulation::
                                           OptionalAdbCommandResult&) { ; },
                            5000);
            (*mAdbInterface)
                    ->runAdbCommand(
                            {"shell", "input", "keyevent", "KEYCODE_WAKEUP"},
                            [this](const android::emulation::
                                           OptionalAdbCommandResult&) { ; },
                            5000);
            TelephonyController::get()->updateTimeAsync();
        }
#ifndef _WIN32
        // When we minimized, we re-enabled the window frame (because Mac won't
        // un-minimize a frameless window). Disable the window frame now, if it
        // should be disabled.
        Qt::WindowFlags flags = mContainer.windowFlags();
#ifdef __linux__
        flags &= ~FRAME_WINDOW_FLAGS_MASK;
        flags |= (hasFrame() ? FRAMED_WINDOW_FLAGS : FRAMELESS_WINDOW_FLAGS);
        flags &= ~Qt::WindowMinimizeButtonHint;
#else  // __APPLE__
        if (hasFrame()) {
            flags &= ~Qt::NoDropShadowWindowHint;
        } else {
            flags &= ~FRAME_WINDOW_FLAGS_MASK;
            flags |= FRAMELESS_WINDOW_FLAGS;
        }
#endif
        mContainer.setWindowFlags(flags);

#if defined(__APPLE__)
        // skip the extra show
#else
        show();
#endif
        // Trigger a ScreenChanged event so the device
        // screen will refresh immediately
        queueSkinEvent(createSkinEventScreenChanged());
#endif  // !_WIN32
    }

    return QWidget::event(ev);
}

void EmulatorQtWindow::stopThread() {
    if (mMainLoopThread) {
        delete mMainLoopThread;
        mMainLoopThread = nullptr;
    }
}

void EmulatorQtWindow::startThread(StartFunction f, int argc, char** argv) {
    if (!mMainLoopThread) {
        // Check for null as arguments to StartFunction
        if (argc && !argv) {
            D("Empty argv passed to a startThread(), returning early");
            return;
        }

        // pass the QEMU main thread's arguments into the crash handler
        std::string arguments = "";
        for (int i = 0; i < argc; ++i) {
            // Check for null in argv
            if (!argv[i]) {
                D("Internal Error: argv[%d] was null, replaced with empty "
                  "string",
                  i);
                argv[i] = strdup("");
            }
            arguments += argv[i];
            arguments += '\n';
        }

        mMainLoopThread = new MainLoopThread(f, argc, argv);
        QObject::connect(mMainLoopThread, &QThread::finished, &mContainer,
                         &EmulatorContainer::close);
        mMainLoopThread->start();
    } else {
        D("mMainLoopThread already started");
    }
}

void EmulatorQtWindow::slot_clearInstance() {
#ifndef __APPLE__
    if (mToolWindow) {
        delete mToolWindow;
        mToolWindow = NULL;
    }
    if (mCarClusterWindow) {
        delete mCarClusterWindow;
        mCarClusterWindow = NULL;
    }
#endif

    skin_winsys_save_window_pos();
    // Force kill any parallel tasks that may be running, as this can make Qt
    // hang on exit.
    System::get()->cleanupWaitingPids();
    if (mMainLoopThread && mMainLoopThread->isRunning()) {
        requestClose();
        mMainLoopThread->wait();
    }
    sInstance.get().reset();
}

void EmulatorQtWindow::slot_blit(SkinSurfaceBitmap* src,
                                 QRect srcRect,
                                 SkinSurfaceBitmap* dst,
                                 QPoint dstPos,
                                 QPainter::CompositionMode op,
                                 QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    if (mBackingSurface && dst == mBackingSurface->bitmap) {
        mBackingBitmapChanged = true;
    }
    dst->drawFrom(src, dstPos, srcRect, op);
}

void EmulatorQtWindow::slot_fill(SkinSurface* s,
                                 QRect rect,
                                 QColor color,
                                 QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    if (mBackingSurface && s == mBackingSurface) {
        mBackingBitmapChanged = true;
    }
    s->bitmap->fill(rect, color);
}

void EmulatorQtWindow::slot_getDevicePixelRatio(double* out_dpr,
                                                QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    // skin_window_create() asks for the ratio before the first showWindow(),
    // when the top-level window has no QWindow yet. Answering 1.0 there made
    // the first native GL subwindow logical-size in device pixels (2/3 of the
    // display area at 150%), so until the guest's first frame only that
    // top-left part kept the skin background. Use the screen the window is
    // about to be shown on instead; QWidget::screen() falls back to the
    // primary screen.
    QWindow* handle = window()->windowHandle();
    QScreen* screen = handle ? handle->screen() : window()->screen();
    *out_dpr = screen ? screen->devicePixelRatio() : 1.0;
}

void EmulatorQtWindow::slot_getScreenDimensions(QRect* out_rect,
                                                QSemaphore* semaphore) {
    D("slot_getScreenDimensions: begin");
    QSemaphoreReleaser semReleaser(semaphore);
    out_rect->setRect(0, 0, 1920, 1080);  // Arbitrary default

    D("slot_getScreenDimensions: Getting screen geometry");
    auto newScreen = window()->windowHandle()
                             ? window()->windowHandle()->screen()
                             : nullptr;
    if (!newScreen) {
        D("Can't get screen geometry. Window is off screen.");
        return;
    }
    // Use availableGeometry() instead of geometry() to get coordinates that are
    // excluding things like a dock, menu bar, etc.
    QRect rect = newScreen->availableGeometry();
    D("slot_getScreenDimensions: Getting screen geometry (done)");
    out_rect->setX(rect.x());
    out_rect->setY(rect.y());

    out_rect->setWidth(rect.width());
    out_rect->setHeight(rect.height());
    D("slot_getScreenDimensions: end");
}

WId EmulatorQtWindow::getWindowId() {
    WId wid = effectiveWinId();
    D("Effective win ID is %lx", wid);
#if defined(__APPLE__)
    wid = (WId)getNSWindow((void*)wid);
    D("After finding parent, win ID is %lx", wid);
#endif
    return wid;
}

void EmulatorQtWindow::slot_getWindowSize(int* ww,
                                          int* hh,
                                          QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    QRect geom = mContainer.geometry();

    *ww = geom.width();
    *hh = geom.height();
}

void EmulatorQtWindow::slot_getWindowPos(int* xx,
                                         int* yy,
                                         QSemaphore* semaphore) {
    // Note that mContainer.x() == mContainer.frameGeometry().x(), which
    // is NOT what we want.

    QSemaphoreReleaser semReleaser(semaphore);
    QRect geom = mContainer.geometry();

    *xx = geom.x();
    *yy = geom.y();
}

void EmulatorQtWindow::slot_windowHasFrame(bool* outValue,
                                           QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    *outValue = hasFrame();
}

void EmulatorQtWindow::slot_getFramePos(int* xx,
                                        int* yy,
                                        QSemaphore* semaphore) {
    // Note that mContainer.x() == mContainer.frameGeometry().x(), which
    // is what we want.

    QSemaphoreReleaser semReleaser(semaphore);
    *xx = mContainer.x();
    *yy = mContainer.y();
}

void EmulatorQtWindow::slot_isWindowFullyVisible(bool* out_value,
                                                 QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    auto newScreen = mContainer.window()->windowHandle()
                             ? mContainer.window()->windowHandle()->screen()
                             : nullptr;
    if (!newScreen) {
        // Window is not on any screen
        *out_value = false;
    } else {
        QRect screenGeo = newScreen->geometry();
        *out_value = screenGeo.contains(mContainer.geometry());
    }
}

void EmulatorQtWindow::slot_isWindowOffScreen(bool* out_value,
                                              QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    auto newScreen = mContainer.window()->windowHandle()
                             ? mContainer.window()->windowHandle()->screen()
                             : nullptr;
    *out_value = (newScreen == nullptr);
}

void EmulatorQtWindow::pollEvent(SkinEvent* event, bool* hasEvent) {
    std::unique_lock<std::mutex> lock(mSkinEventQueueMtx);
    if (mSkinEventQueue.empty()) {
        *hasEvent = false;
    } else {
        *event = mSkinEventQueue.front();
        mSkinEventQueue.pop_front();
        *hasEvent = true;
    }
}

void EmulatorQtWindow::queueSkinEvent(SkinEvent event) {
    const auto eventType = event.type;
    const auto rotationEventLayout =
            (eventType == kEventLayoutRotate)
                    ? makeOptional(event.u.layout_rotation.rotation)
                    : kNullopt;

    const std::lock_guard<std::mutex> lock(mSkinEventQueueMtx);
    const bool firstEvent = mSkinEventQueue.empty();

    // For the following events, only the "last" example of said event
    // matters, so ensure that there is only one of them in the queue at a
    // time. Additionaly the screen changed event processing is very slow,
    // so let's not generate too many of them.
    bool replaced = false;

    switch (eventType) {
        case kEventScrollBarChanged:
        case kEventZoomedWindowResized:
        case kEventScreenChanged: {
            const auto i =
                    std::find_if(mSkinEventQueue.begin(), mSkinEventQueue.end(),
                                 [eventType](const SkinEvent& ev) {
                                     return ev.type == eventType;
                                 });
            if (i != mSkinEventQueue.end()) {
                *i = std::move(event);
                replaced = true;
            }
        } break;

        default:
            break;
    }

    if (!replaced) {
        mSkinEventQueue.push_back(std::move(event));
    }

    const auto uiAgent = mToolWindow->getUiEmuAgent();
    if (firstEvent && uiAgent && uiAgent->userEvents &&
        uiAgent->userEvents->onNewUserEvent) {
        // We know that as soon as emulator starts processing user events
        // it processes them until there are none. So we should only notify it
        // if this event is the first one.
        uiAgent->userEvents->onNewUserEvent();
    }

    if (getConsoleAgents()->settings->android_cmdLineOptions()->grpc_ui) {
        // Restrict outbound resizable config signal emissions to fishtank.
        if (eventType == kEventSetDisplayActiveConfig) {
            emit resizableConfigChanged(event.u.display_active_config);
        }
    }

    if (rotationEventLayout) {
        emit(layoutChanged(*rotationEventLayout));
    }
}

void EmulatorQtWindow::slot_updateRotation(SkinRotation rotation) {
    mOrientation = rotation;
    mScreenMaskChanged = true;
    emit(layoutChanged(rotation));

    // update the extended ui device pose page for foldable
    // or non-fodable
    mToolWindow->updateFoldableButtonVisibility();
    fixScale();
}

void EmulatorQtWindow::slot_releaseBitmap(SkinSurface* s,
                                          QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    if (mBackingSurface == s) {
        mBackingSurface = NULL;
        mBackingBitmapChanged = true;
    }
    delete s->bitmap;
    delete s;
}

void EmulatorQtWindow::slot_requestClose(QSemaphore* semaphore) {
    // The lean toolbar keeps console and main-loop exits immediate; only its
    // own close paths start the safe shutdown.
    if (!mToolWindow->isLeanToolbar() && isMainThreadRunning() &&
        !mToolWindow->closeButtonClicked()) {
        mToolWindow->on_close_button_clicked();
    }
    QSemaphoreReleaser semReleaser(semaphore);
    mToolWindow->shouldClose();
    if (isMainThreadRunning()) {
        if (!getConsoleAgents()->settings->android_cmdLineOptions()->grpc_ui) {
            // TODO: This should be a backend operation, and moved out of Qt codebase.
            using android::snapshot::Snapshotter;
            Snapshotter::get().stopVulkanAppsIfApplicable();
        }
        queueQuitEvent();
    }

    // In Fishtank mode, we should notify the emulator to shut down
    // if we are not in embedded mode.
    auto opts = getConsoleAgents()->settings->android_cmdLineOptions();
    if (opts->grpc_ui && !opts->qt_hide_window) {
        if (getConsoleAgents()->vm) {
            getConsoleAgents()->vm->vmShutdown();
        }
    }

    System::get()->waitAndKillSelf();
}

void EmulatorQtWindow::slot_requestUpdate(QRect rect, QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    if (!mBackingSurface)
        return;
    if (!mBackingSurface->bitmap)
        return;
    if (!mBackingSurface->bitmap->size().width() ||
        !mBackingSurface->bitmap->size().height())
        return;

    QRect r(rect.x() * mBackingSurface->w /
                    mBackingSurface->bitmap->size().width(),
            rect.y() * mBackingSurface->h /
                    mBackingSurface->bitmap->size().height(),
            rect.width() * mBackingSurface->w /
                    mBackingSurface->bitmap->size().width(),
            rect.height() * mBackingSurface->h /
                    mBackingSurface->bitmap->size().height());
    update(r);
}

void EmulatorQtWindow::slot_setDeviceGeometry(QRect rect,
                                              QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    mDeviceGeometry = rect;
}

void EmulatorQtWindow::slot_setWindowPos(int x, int y, QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    mContainer.move(x, y);
}

void EmulatorQtWindow::slot_setWindowSize(int w, int h, QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    if (ToolWindow::inWindowNav()) {
        w += 54;
    }
    mContainer.resize(w, h);
}

void EmulatorQtWindow::slot_paintWindowOverlayForResize(int mouseX,
                                                        int mouseY,
                                                        QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    mOverlay.paintForResize(mouseX, mouseY);
}

void EmulatorQtWindow::slot_clearWindowOverlay(QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    mOverlay.hide();
}

void EmulatorQtWindow::slot_setWindowOverlayForResize(int whichCorner,
                                                      QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    mOverlay.showForResize(whichCorner);
}

void EmulatorQtWindow::slot_setWindowCursorResize(int whichCorner,
                                                  QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    mContainer.setCursor((whichCorner == 0 || whichCorner == 2)
                                 ? Qt::SizeFDiagCursor
                                 : Qt::SizeBDiagCursor);
}

void EmulatorQtWindow::slot_setWindowCursorNormal(QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    mContainer.setCursor(getCursorShape(/*mouseGrabbed*/ false));
}

void EmulatorQtWindow::slot_setWindowIcon(const unsigned char* data,
                                          int size,
                                          QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    QPixmap image;
    image.loadFromData(data, size);
    QIcon icon(image);
    QApplication::setWindowIcon(icon);
}

void EmulatorQtWindow::slot_setWindowTitle(QString title,
                                           QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    mBaseWindowTitle = title;
    updateWindowTitle();

    (*mAdbInterface)->setSerialNumberPort(
            getConsoleAgents()->settings->android_serial_number_port());
}

void EmulatorQtWindow::slot_showWindow(SkinSurface* surface,
                                       QRect rect,
                                       QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    if (mClosed || mHiddenForSafeShutdown) {
        return;
    }
    if (getConsoleAgents()
                ->settings->android_cmdLineOptions()
                ->qt_hide_window) {
        return;
    }
    if (surface != mBackingSurface) {
        if (mBackingSurface) {
            QSize backingSize = mBackingSurface->bitmap->size();
        }
        mBackingBitmapChanged = true;
        mBackingSurface = surface;
        if (mBackingSurface) {
            QSize backingSize = mBackingSurface->bitmap->size();
        }
    }

    showNormal();
    setFixedSize(rect.size());

    const int guestMinimumWidth = mContainer.minimumWidth() -
                                  (ToolWindow::inWindowNav() ? 54 : 0);
    if (rect.width() < guestMinimumWidth ||
        rect.height() < mContainer.minimumHeight()) {
        // We're trying to go smaller than the container will allow.
        // Increase our size to the container's minimum.
        double horizIncreaseFactor =
                (double)guestMinimumWidth / rect.width();
        double vertIncreaseFactor =
                (double)(mContainer.minimumHeight()) / rect.height();
        if (horizIncreaseFactor > vertIncreaseFactor) {
            rect.setWidth(guestMinimumWidth);
            rect.setHeight((int)(horizIncreaseFactor * rect.height()));
        } else {
            rect.setWidth((int)(vertIncreaseFactor * rect.width()));
            rect.setHeight(mContainer.minimumHeight());
        }
    }

    // If this was the result of a zoom, don't change the overall window size,
    // and adjust the scroll bars to reflect the desired focus point.
    if (mInZoomMode && mNextIsZoom) {
        mContainer.stopResizeTimer();
        recenterFocusPoint();
    } else if (!mNextIsZoom) {
        QSize containerSize = rect.size();
        if (ToolWindow::inWindowNav()) {
            containerSize.rwidth() += 54;
        }
        mContainer.resize(containerSize);
    }
    mNextIsZoom = false;

    maskWindowFrame();
    show();

    // Zooming forces the scroll bar to be visible for sizing purposes. They
    // should never be shown when not in zoom mode, and should only show when
    // necessary when in zoom mode.
    if (mInZoomMode) {
        mContainer.setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        mContainer.setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    } else {
        mContainer.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        mContainer.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    // If the user isn't using an x86 AVD, make sure it's because their machine
    // doesn't support CPU acceleration. If it does, recommend switching to an
    // x86 AVD. This cannot be done on the construction of the window since the
    // Qt UI thread has not been properly initialized yet.
    if (mFirstShowWindowCall) {
        showWin32DeprecationWarning();
        showAvdArchWarning();
        checkShouldShowGpuWarning();
        showGpuWarning();
        checkAdbVersionAndWarn();
        checkNestedAndWarn();
        displayCheckWarnings();
        mFirstShowWindowCall = false;
    }

    hostImeUpdateCursor();
}

void EmulatorQtWindow::onScreenChanged(QScreen* newScreen) {
    if (newScreen != mCurrentScreen) {
        D("%s: kEventScreenChanged", __FUNCTION__);
        queueSkinEvent(createSkinEventScreenChanged());
        mCurrentScreen = newScreen;
    }
}

void EmulatorQtWindow::onScreenConfigChanged() {
    auto newScreen = window()->windowHandle()
                             ? window()->windowHandle()->screen()
                             : nullptr;
    if (newScreen != mCurrentScreen) {
        D("%s: kEventScreenChanged", __FUNCTION__);
        queueSkinEvent(createSkinEventScreenChanged());
        mCurrentScreen = newScreen;
    }
}

void EmulatorQtWindow::showEvent(QShowEvent* event) {
    mStartupTimer.stop();
    mStartupDone = true;

    if (mClosed) {
        event->ignore();
        return;
    }
    if (mFirstShowEvent) {
        if (getConsoleAgents()->settings->hw()->test_quitAfterBootTimeOut > 0) {
            android_test_start_boot_complete_timer(
                    getConsoleAgents()
                            ->settings->hw()
                            ->test_quitAfterBootTimeOut);
        }
        mFirstShowEvent = false;
    }
}

void EmulatorQtWindow::slot_horizontalScrollChanged(int value) {
    simulateScrollBarChanged(value, mContainer.verticalScrollBar()->value());
    hostImeUpdateCursor();
}

void EmulatorQtWindow::slot_verticalScrollChanged(int value) {
    simulateScrollBarChanged(mContainer.horizontalScrollBar()->value(), value);
    hostImeUpdateCursor();
}

void EmulatorQtWindow::slot_scrollRangeChanged(int min, int max) {
    simulateScrollBarChanged(mContainer.horizontalScrollBar()->value(),
                             mContainer.verticalScrollBar()->value());
}

void EmulatorQtWindow::screenshot() {
    QString savePath = getScreenshotSaveDirectory();
    if (savePath.isEmpty()) {
        showErrorDialog(tr("The screenshot save location is not set.<br/>"
                           "Check the settings page and ensure the directory "
                           "exists and is writeable."),
                        tr("Screenshot"));
        return;
    }

    int displayId = 0;
    if (getConsoleAgents()->settings->android_cmdLineOptions()->grpc_ui) {
        // Fishtank uses the display id of the last screenshot obtained from streamScreenshot.
        displayId = mActiveDisplayId;
    } else {
        const bool pixel_fold = android_foldable_is_pixel_fold();
        if (pixel_fold) {
            if (android_foldable_is_folded()) {
                displayId = android_foldable_pixel_fold_second_display_id();
            }
        }
    }

    const auto uiAgent = mToolWindow->getUiEmuAgent();
    if (!uiAgent->record->doSnap(savePath.toStdString().c_str(), displayId)) {
        showErrorDialog(tr("Screenshot failed"), tr("Screenshot"));
    } else {
        // Display the flash animation immediately as feedback - if it fails, an
        // error dialog will indicate as such.
        mOverlay.showAsFlash();
    }
}
void EmulatorQtWindow::slot_installCanceled() {
    if (mApkInstallCommand && mApkInstallCommand->inFlight()) {
        mApkInstallCommand->cancel();
    }
}

void EmulatorQtWindow::runAdbInstall(const QString& path) {
    if (mApkInstallCommand && mApkInstallCommand->inFlight()) {
        // Modal dialogs should prevent this.
        return;
    }

    // Show a dialog so the user knows something is happening
    mInstallDialog->show();

    mApkInstallCommand = mApkInstaller->install(
            path.toStdString(),
            [this](ApkInstaller::Result result, std::string errorString) {
                runOnUiThread([this, result, errorString] {
                    installDone(result, errorString);
                });
            });
}

void EmulatorQtWindow::installDone(ApkInstaller::Result result,
                                   std::string_view errorString) {
    mInstallDialog->hide();

    QString msg;
    switch (result) {
        case ApkInstaller::Result::kSuccess:
            return;

        case ApkInstaller::Result::kOperationInProgress:
            msg =
                    tr("Another APK install is already in progress.<br/>"
                       "Please try again after it completes.");
            break;

        case ApkInstaller::Result::kApkPermissionsError:
            msg =
                    tr("Unable to read the given APK.<br/>"
                       "Ensure that the file is readable.");
            break;

        case ApkInstaller::Result::kAdbConnectionFailed:
            msg =
                    tr("Failed to start adb.<br/>"
                       "Check settings to verify your chosen adb path is "
                       "valid.");
            break;

        case ApkInstaller::Result::kInstallFailed:
            msg = tr("The APK failed to install.<br/> Error: %1")
                          .arg(c_str(errorString).get());
            break;

        default:
            msg = tr("There was an unknown error while installing the APK.");
    }

    showErrorDialog(msg, tr("APK Installer"));
}

void EmulatorQtWindow::runAdbPush(const QList<QUrl>& urls) {
    std::vector<std::pair<std::string, std::string>> file_paths;
    std::string_view remoteDownloadsDir =
            avdInfo_getApiLevel(getConsoleAgents()->settings->avdInfo()) > 10
                    ? kRemoteDownloadsDir
                    : kRemoteDownloadsDirApi10;
    for (const auto& url : urls) {
        std::string remoteFile = PathUtils::join(remoteDownloadsDir.data(),
                                                 url.fileName().toStdString());
        file_paths.push_back(
                std::make_pair(url.toLocalFile().toStdString(), remoteFile));
    }

    mFilePusher->pushFiles(file_paths);
}

void EmulatorQtWindow::slot_adbPushCanceled() {
    mFilePusher->cancel();
}

void EmulatorQtWindow::slot_showMessage(QString text,
                                        Ui::OverlayMessageType messageType,
                                        int timeoutMs) {
    if (!getConsoleAgents()->settings->android_cmdLineOptions()->qt_hide_window)
        mContainer.messageCenter().addMessage(text, messageType, timeoutMs);
}

void EmulatorQtWindow::slot_showMessageWithDismissCallback(
        QString text,
        Ui::OverlayMessageType messageType,
        QString dismissText,
        RunOnUiThreadFunc func,
        int timeoutMs) {
    if (!getConsoleAgents()
                 ->settings->android_cmdLineOptions()
                 ->qt_hide_window) {
        auto msg = mContainer.messageCenter().addMessage(text, messageType,
                                                         timeoutMs);
        msg->setDismissCallback(dismissText, std::move(func));
    }
}

void EmulatorQtWindow::adbPushProgress(double progress, bool done) {
    if (done) {
        mPushDialog->hide();
        return;
    }

    mPushDialog->setValue(progress * kPushProgressBarMax);
    mPushDialog->show();
}

void EmulatorQtWindow::adbPushDone(std::string_view filePath,
                                   FilePusher::Result result) {
    QString msg;
    switch (result) {
        case FilePusher::Result::Success:
            return;
        case FilePusher::Result::ProcessStartFailure:
            msg = tr("Could not launch process to copy %1")
                          .arg(c_str(filePath).get());
            break;
        case FilePusher::Result::FileReadError:
            msg = tr("Could not locate %1").arg(c_str(filePath).get());
            break;
        case FilePusher::Result::AdbPushFailure:
            msg = tr("'adb push' failed for %1").arg(c_str(filePath).get());
            break;
        default:
            msg = tr("Could not copy %1").arg(c_str(filePath).get());
    }
    showErrorDialog(msg, tr("File Copy"));
}

void EmulatorQtWindow::doResize(const QSize& size, bool isKbdShortcut) {
    if (mClosed) {
        return;
    }
    if (!mBackingSurface) {
        return;
    }
    int originalWidth = mBackingSurface->bitmap->size().width();
    int originalHeight = mBackingSurface->bitmap->size().height();

    auto newSize = QSize(originalWidth, originalHeight)
                           .scaled(size, Qt::KeepAspectRatio);

    // Make sure the new size is always a little bit smaller than the
    // screen to prevent keyboard shortcut scaling from making a window
    // too large for the screen, which can result in the showing of the
    // scroll bars. This is not an issue when resizing by dragging the
    // corner because the OS will prevent too large a window.
    if (isKbdShortcut) {
        QRect screenDimensions;
        slot_getScreenDimensions(&screenDimensions);

        const int screenWidth = screenDimensions.width() -
                                (ToolWindow::inWindowNav() ? 54 : 0);
        if (newSize.width() > screenWidth ||
            newSize.height() > screenDimensions.height()) {
            newSize.scale(QSize(screenWidth, screenDimensions.height()),
                          Qt::KeepAspectRatio);
        }
    }

    double widthScale = (double)newSize.width() / (double)originalWidth;
    double heightScale = (double)newSize.height() / (double)originalHeight;

    // On HiDPI screen, newSize is in Qt logical pixel, whle originalWidth/
    // originalHeight are in actual pixel (logical pixel * pixel ratio).
    // When HiDPI scaling is disabled, we need to multiply the pixel ratio to
    // widthScale/heightScale to make the scale correct.
    if (getConsoleAgents()
                ->settings->android_cmdLineOptions()
                ->no_hidpi_scaling) {
        double dpr = 1.0;
        slot_getDevicePixelRatio(&dpr);
        widthScale *= dpr;
        heightScale *= dpr;
    }

    simulateSetScale(std::max(.2, std::min(widthScale, heightScale)));

    maskWindowFrame();
#ifdef __APPLE__
    // To fix issues when resizing + linking against macos sdk 11.
    queueSkinEvent(createSkinEventScreenChanged());
#endif
}

void EmulatorQtWindow::resizeAndChangeAspectRatio(bool isFolded) {
    QRect windowGeo = this->geometry();
    if (!mBackingSurface) {
        VERBOSE_INFO(foldable,
                     "backing surface not ready, cancel window adjustment");
        return;
    }

    const bool is_pixel_fold = android_foldable_is_pixel_fold();
    if (is_pixel_fold) {
        if (isFolded) {
            setFoldedSkin();
        } else {
            if (resizableEnabled34()) {
                PresetEmulatorSizeType activeConfig =
                        getResizableActiveConfigId();
                if (activeConfig == 1) {
                    restoreSkin();
                }
            } else {
                restoreSkin();
            }
        }
        return;
    }

    QSize backingSize = mBackingSurface->bitmap->size();
    float scale = (float)windowGeo.width() / (float)backingSize.width();
    int displayX = getConsoleAgents()->settings->hw()->hw_lcd_width;
    int displayY = getConsoleAgents()->settings->hw()->hw_lcd_height;

    if (resizableEnabled()) {
        PresetEmulatorSizeType activeConfig = getResizableActiveConfigId();
        PresetEmulatorSizeInfo info;
        if (getResizableConfig(activeConfig, &info)) {
            displayX = info.width;
            displayY = info.height;
        }
    }

    if (isFolded) {
        int displayXFolded;
        int displayYFolded;
        android_foldable_get_folded_area(nullptr, nullptr, &displayXFolded,
                                         &displayYFolded);
        switch (mOrientation) {
            default:
            case SKIN_ROTATION_180:
            case SKIN_ROTATION_0:
                if (backingSize.width() == displayXFolded &&
                    backingSize.height() == displayYFolded) {
                    break;
                }
                windowGeo.setWidth((int)(displayXFolded * scale));
                windowGeo.setHeight((int)(displayYFolded * scale));
                backingSize.setWidth(displayXFolded);
                backingSize.setHeight(displayYFolded);
                break;
            case SKIN_ROTATION_90:
            case SKIN_ROTATION_270:
                if (backingSize.width() == displayYFolded &&
                    backingSize.height() == displayXFolded) {
                    break;
                }
                backingSize.setWidth(displayYFolded);
                backingSize.setHeight(displayXFolded);
                windowGeo.setWidth((int)(displayYFolded * scale));
                windowGeo.setHeight((int)(displayXFolded * scale));
                break;
        }
    } else {
        switch (mOrientation) {
            default:
            case SKIN_ROTATION_0:
            case SKIN_ROTATION_180:
                if (backingSize.width() == displayX &&
                    backingSize.height() == displayY) {
                    break;
                }
                windowGeo.setWidth((int)(displayX * scale));
                windowGeo.setHeight((int)(displayY * scale));
                backingSize.setWidth(displayX);
                backingSize.setHeight(displayY);
                break;
            case SKIN_ROTATION_90:
            case SKIN_ROTATION_270:
                if (backingSize.width() == displayY &&
                    backingSize.height() == displayX) {
                    break;
                }
                windowGeo.setWidth((int)(displayY * scale));
                windowGeo.setHeight((int)(displayX * scale));
                backingSize.setWidth(displayY);
                backingSize.setHeight(displayX);
                break;
        }
    }
    setDisplayRegionAndUpdate(0, 0, backingSize.width(), backingSize.height());
    simulateSetScale(std::max(.2, (double)scale));
    QRect containerGeo = mContainer.geometry();
    mContainer.setGeometry(containerGeo.x(), containerGeo.y(),
                           windowGeo.width() +
                                   (ToolWindow::inWindowNav() ? 54 : 0),
                           windowGeo.height());
}

void EmulatorQtWindow::resizeAndChangeAspectRatio(int x,
                                                  int y,
                                                  int w,
                                                  int h,
                                                  bool ignoreOrientation) {
    if (!mBackingSurface) {
        return;
    }
    QRect windowGeo = this->geometry();
    QSize backingSize = mBackingSurface->bitmap->size();
    float scale = (float)windowGeo.width() / (float)backingSize.width();
    if (!ignoreOrientation) {
        switch (mOrientation) {
            case SKIN_ROTATION_90:
            case SKIN_ROTATION_270:
                std::swap(w, h);
                std::swap(x, y);
                break;
            case SKIN_ROTATION_0:
            case SKIN_ROTATION_180:
            default:
                break;
        }
    }
    windowGeo.setWidth((int)(w * scale));
    windowGeo.setHeight((int)(h * scale));
    backingSize.setWidth(w);
    backingSize.setHeight(h);
    setDisplayRegionAndUpdate(x, y, backingSize.width(), backingSize.height());
    simulateSetScale(std::max(.2, (double)scale));
    QRect containerGeo = mContainer.geometry();
    mContainer.setGeometry(containerGeo.x(), containerGeo.y(),
                           windowGeo.width() +
                                   (ToolWindow::inWindowNav() ? 54 : 0),
                           windowGeo.height());
}

SkinMouseButtonType EmulatorQtWindow::getSkinMouseButton(
        const QMouseEvent* event) const {
    switch (event->button()) {
        case Qt::LeftButton:
            return kMouseButtonLeft;
        case Qt::RightButton:
            return kMouseButtonRight;
        case Qt::MiddleButton:
            return kMouseButtonMiddle;
        default:
            return kMouseButtonLeft;
    }
}

void EmulatorQtWindow::handleMouseEvent(SkinEventType type,
                                        SkinMouseButtonType button,
                                        const QPointF& posF,
                                        const QPointF& gPosF,
                                        bool skipSync,
                                        bool sendRelativeMouseCoordinates) {
    if (button == kMouseButtonRight) {
        const bool shouldTranslateMouseClickToTouch =
                (!feature_is_enabled(kFeature_VirtioMouse) &&
                 !feature_is_enabled(kFeature_VirtioTablet));
        if (shouldTranslateMouseClickToTouch) {
            const bool down = (type == kEventMouseButtonDown);
            SkinEvent skin_event =
                    createSkinEvent(down ? kEventKeyDown : kEventKeyUp);
            skin_event.u.key.keycode = LINUX_KEY_BACK;
            skin_event.u.key.mod = 0;
            queueSkinEvent(std::move(skin_event));
            return;
        }
    }

    QPoint pos((int)posF.x(), (int)posF.y());
    QPoint gPos((int)gPosF.x(), (int)gPosF.y());
    if (type == kEventMouseButtonDown) {
        mToolWindow->reportMouseButtonDown();
    }

    SkinEvent skin_event = createSkinEvent(type);
    skin_event.u.mouse.button = button;
    skin_event.u.mouse.skip_sync = skipSync;
    skin_event.u.mouse.send_relative_coordinates = sendRelativeMouseCoordinates;
    skin_event.u.mouse.x = pos.x();
    skin_event.u.mouse.y = pos.y();
    skin_event.u.mouse.x_global = gPos.x();
    skin_event.u.mouse.y_global = gPos.y();

    skin_event.u.mouse.xrel = pos.x() - mPrevMousePosition.x();
    skin_event.u.mouse.yrel = pos.y() - mPrevMousePosition.y();
    skin_event.u.mouse.display_id = 0;
    mPrevMousePosition = pos;

    queueSkinEvent(std::move(skin_event));
}

// Stores all the information of a pen event and
// adds the skin event to the queue
void EmulatorQtWindow::handlePenEvent(SkinEventType type,
                                      const QTabletEvent* event,
                                      bool skipSync) {
    SkinEvent skin_event = createSkinEvent(type);
    skin_event.u.pen.tracking_id = event->uniqueId();
    skin_event.u.pen.pressure =
            (int)(event->pressure() * MTS_PRESSURE_RANGE_MAX);
    skin_event.u.pen.orientation =
            penOrientation(tiltToRotation(event->xTilt(), event->yTilt()));
    skin_event.u.pen.button_pressed =
            ((event->buttons() & Qt::RightButton) == Qt::RightButton);
    skin_event.u.pen.rubber_pointer =
            (event->pointerType() == QPointingDevice::PointerType::Eraser);
    skin_event.u.pen.x = event->pos().x();
    skin_event.u.pen.y = event->pos().y();
    skin_event.u.pen.x_global = event->globalPos().x();
    skin_event.u.pen.y_global = event->globalPos().y();
    skin_event.u.pen.skip_sync = skipSync;

    queueSkinEvent(std::move(skin_event));
}

void EmulatorQtWindow::handleMouseWheelEvent(int delta,
                                             Qt::Orientation orientation) {
    SkinEvent skin_event = createSkinEvent(kEventMouseWheel);
    skin_event.u.wheel.x_delta = 0;
    skin_event.u.wheel.y_delta = 0;
    skin_event.u.wheel.display_id = 0;
    if (orientation == Qt::Horizontal) {
        skin_event.u.wheel.x_delta = delta;
    } else {
        skin_event.u.wheel.y_delta = delta;
    }

    queueSkinEvent(std::move(skin_event));
}

int EmulatorQtWindow::generateModData(
        const Qt::KeyboardModifiers& eventModifiers) {
    int mod = 0;

    if (eventModifiers & Qt::ShiftModifier) {
        mod |= kKeyModLShift;
    }
    if (eventModifiers & Qt::ControlModifier) {
        mod |= kKeyModLCtrl;
    }
    if (eventModifiers & Qt::AltModifier) {
        mod |= kKeyModLAlt;
    }
    if (eventModifiers & Qt::MetaModifier) {
        mod |= kKeyModLMeta;
    }

    return mod;
}

void EmulatorQtWindow::forwardKeyEventToEmulator(SkinEventType type,
                                                 const QKeyEvent& event) {
    SkinEvent skin_event = createSkinEvent(type);
    SkinEventKeyData& keyData = skin_event.u.key;
    bool isModifier = false;
    keyData.keycode = convertKeyCode(event.key(), isModifier);
    if (android::featurecontrol::isEnabled(
                android::featurecontrol::QtRawKeyboardInput)) {
        if (!isModifier) {
            std::optional<int> result = android::qt::getUnmodifiedQtKey(event);
            if (result && *result != event.key()) {
                keyData.keycode = convertKeyCode(*result, isModifier);
            }
        }
    }
    if (keyData.keycode == -1) {
        D("Failed to convert key for event key %d", event.key());
        return;
    }

    if (isModifier) {
        if (type == kEventKeyDown) {
            mHeldModifiers.insert(keyData.keycode);
        } else if (type == kEventKeyUp) {
            mHeldModifiers.erase(keyData.keycode);
        }
    }

    Qt::KeyboardModifiers modifiers = event.modifiers();
    keyData.mod = generateModData(modifiers);

    queueSkinEvent(std::move(skin_event));
}

void EmulatorQtWindow::handleKeyEvent(SkinEventType type,
                                      const QKeyEvent& event) {
    // TODO(liyl): Make this shortcut configurable instead of hard-coding it
    // inside the code.
    if (!android::featurecontrol::isEnabled(
                android::featurecontrol::VirtioDualModeMouse)) {
        if (isReleaseMouseShortcut(type, event)) {
            D("%s: mouse released\n", __func__);
            releaseMouse();

            queueSkinEvent(
                    createMouseTrackingSkinEvent(kEventMouseStopTracking));

            mMouseGrabbed = false;
            updateWindowTitle();
        }
    }

    if (!mForwardShortcutsToDevice && mInZoomMode) {
        if (event.key() == Qt::Key_Control) {
            if (type == kEventKeyUp) {
                raise();
                mOverlay.showForZoomUserHidden();
            }
        }
    }

    bool sendRawKeyboardInputToGuest = android::featurecontrol::isEnabled(
            android::featurecontrol::QtRawKeyboardInput);
    // Not in host IME text mode, where Ctrl starts Ctrl+letter keys.
    if (!mForwardShortcutsToDevice && !mInZoomMode && !mHostImeTextMode &&
        !sendRawKeyboardInputToGuest && event.key() == Qt::Key_Control &&
        (event.modifiers() == Qt::ControlModifier ||
         event.modifiers() == (Qt::ControlModifier | Qt::ShiftModifier))) {
        if (type == kEventKeyDown && !mDisablePinchToZoom) {
            if (mToolWindow->getUiEmuAgent()
                        ->multiDisplay->isMultiDisplayEnabled() == false) {
                raise();
                D("%s: Using default display for multi touch\n", __FUNCTION__);
                mOverlay.showForMultitouch(
                        event.modifiers() == Qt::ControlModifier,
                        deviceGeometry().center());
            } else {
                QPoint mousePosition = mapFromGlobal(QCursor::pos());
                uint32_t w = 0;
                uint32_t h = 0;
                mToolWindow->getUiEmuAgent()
                        ->multiDisplay->getCombinedDisplaySize(&w, &h);
                double widthRatio = (double)geometry().width() / (double)w;
                double heightRatio = (double)geometry().height() / (double)h;
                int displayId = 0;
                const bool pixel_fold = android_foldable_is_pixel_fold();
                if (pixel_fold) {
                    if (android_foldable_is_folded()) {
                        displayId =
                                android_foldable_pixel_fold_second_display_id();
                    }
                }

                int pos_x, pos_y, startId = -1;
                uint32_t width, height, id;
                while (mToolWindow->getUiEmuAgent()
                               ->multiDisplay->getNextMultiDisplay(
                                       startId, &id, &pos_x, &pos_y, &width,
                                       &height, nullptr, nullptr, nullptr)) {
                    if (id != displayId) {
                        startId = id;
                        continue;
                    }
                    QRect r(static_cast<int>(pos_x * widthRatio),
                            static_cast<int>(pos_y * heightRatio),
                            static_cast<int>(width * widthRatio),
                            static_cast<int>(height * heightRatio));
                    if (r.contains(mousePosition, true)) {
                        D("%s: using display %d for multi touch\n",
                          __FUNCTION__, id);
                        mOverlay.showForMultitouch(
                                event.modifiers() == Qt::ControlModifier,
                                r.center());
                        break;
                    }
                    startId = id;
                }
            }
        }
    }

    bool qtEvent = false;

    if (!mForwardShortcutsToDevice) {
        qtEvent = mToolWindow->handleQtKeyEvent(
                event, QtKeyEventSource::EmulatorWindow);
    }

    if (getConsoleAgents()->settings->use_keycode_forwarding()) {
        return;
    }

    if (!qtEvent) {
        forwardKeyEventToEmulator(type, event);
        if (type == kEventKeyDown && event.text().length() > 0) {
            Qt::KeyboardModifiers mods = event.modifiers();
            Qt::KeyboardModifiers modsWithoutShiftAndKeypad =
                    mods & ~(Qt::ShiftModifier | Qt::KeypadModifier);
            if (modsWithoutShiftAndKeypad == 0) {
                // The key event generated text without Ctrl, Alt, etc.
                // Send an additional TextInput event to the emulator.
                SkinEvent skin_event = createSkinEvent(kEventTextInput);
                skin_event.u.text.down = false;
                strncpy((char*)skin_event.u.text.text,
                        (const char*)event.text().toUtf8().constData(),
                        sizeof(skin_event.u.text.text) - 1);
                // Ensure the event's text is 0-terminated
                skin_event.u.text.text[sizeof(skin_event.u.text.text) - 1] = 0;
                queueSkinEvent(std::move(skin_event));
            } else {
                // It handles key combination inputs with Ctrl, Alt, etc.
                // In this case, event.text() doesn't contain printable
                // character.
                // Send a keycode and modifiers as TextInput event.
                SkinEvent skin_event = createSkinEvent(kEventTextInput);
                skin_event.u.text.down = false;
                SkinEventTextInputData& textData = skin_event.u.text;

                bool isModifier = false;
                textData.keycode = convertKeyCode(event.key(), isModifier);
                if (textData.keycode == -1) {
                    D("Failed to convert key for event key %d", event.key());
                    return;
                }

                textData.mod = generateModData(mods);

                queueSkinEvent(std::move(skin_event));
            }
        }
    }
}

// Host IME text mode. While the guest input method (ModAVD Host Input) says
// an editable text field has the focus, the container takes input method
// events, and keyPressEvent() sends printable text ('C') and editing keys
// ('K') over the modavd_ime pipe instead of to the guest keyboard. All other
// keys take the usual path. A key's release, and its auto-repeats, take the
// path its press took, so switching modes never leaves a key held down.

void EmulatorQtWindow::onHostImeState(bool connected,
                                      bool active,
                                      uint32_t inputType,
                                      bool hasCursor,
                                      QRect guestCursor,
                                      unsigned changes) {
    mHostImeInputType = inputType;
    mHostImeHasCursor = hasCursor;
    mHostImeGuestCursor = guestCursor;
#ifdef _WIN32
    const bool textMode = connected && active;
#else
    // Elsewhere the native event filters still forward keys themselves with
    // keycode forwarding on, so they would reach the guest twice.
    const bool textMode = false;
#endif
    if (textMode != mHostImeTextMode) {
        setHostImeTextMode(textMode);
    } else if (mHostImeTextMode &&
               (changes & (ModavdImePipe::kFocusChanged |
                           ModavdImePipe::kCursorChanged))) {
        QGuiApplication::inputMethod()->update(Qt::ImHints |
                                               Qt::ImCursorRectangle);
    }
}

void EmulatorQtWindow::setHostImeTextMode(bool on) {
    if (on == mHostImeTextMode) {
        return;
    }
    QInputMethod* inputMethod = QGuiApplication::inputMethod();
    if (on) {
        // Once the input method is on it may take the releases of keys held
        // down the usual way, which would leave them down in the guest:
        // release them now and let the rest of their presses go nowhere.
        for (auto it = mHostImeHeld.begin(); it != mHostImeHeld.end();) {
            if (it->second.path != HostImePath::Raw) {
                ++it;
            } else if (!hostImeKeyIsDown(it->second)) {
                it = mHostImeHeld.erase(it);
            } else {
                hostImeReleaseRawKey(it->second);
                it->second.path = HostImePath::Dropped;
                ++it;
            }
        }
        mHostImeTextMode = true;
        mHostImeComposing = false;
        mContainer.setAttribute(Qt::WA_InputMethodEnabled, true);
    } else {
        // Drop an unfinished composition. Qt may hand it over as committed
        // text first; handleInputMethodEvent() ignores it. Only when the
        // composition is ours: reset() acts on whichever widget has one.
        if (QGuiApplication::focusObject() == &mContainer) {
            mHostImeResetting = true;
            inputMethod->reset();
            mHostImeResetting = false;
        }
        // Keys still held since text mode started go the usual way again
        // from their next auto-repeat on.
        for (auto it = mHostImeHeld.begin(); it != mHostImeHeld.end();) {
            if (it->second.path != HostImePath::Dropped) {
                ++it;
            } else if (!hostImeKeyIsDown(it->second)) {
                it = mHostImeHeld.erase(it);
            } else {
                it->second.path = HostImePath::Raw;
                ++it;
            }
        }
        mHostImeTextMode = false;
        mHostImeComposing = false;
        mContainer.setAttribute(Qt::WA_InputMethodEnabled, false);
    }
    inputMethod->update(Qt::ImEnabled | Qt::ImHints | Qt::ImCursorRectangle);
    ModavdImePipe::setTextMode(on);
    D("%s: host IME text mode %s", __func__, on ? "on" : "off");
}

// Returns true when the press is handled here, false when it takes the usual
// path. A new press of a key decides its path; an auto-repeat, or a held key
// Qt reports again as a new press after a modifier changes, keeps it.
// Windows says which is which for each WM_KEYDOWN; Qt's isAutoRepeat() also
// calls a new press an auto-repeat when Qt missed the last release.
bool EmulatorQtWindow::hostImeKeyPress(const QKeyEvent& event) {
    const quint32 id = hostImeKeyId(event);
    HostImeNativeKey native = {};
    const bool hasNative = hostImeTakeNativeKey(event, &native);
    const bool newPress = hasNative ? native.newPress : !event.isAutoRepeat();
    // Keycode forwarding sent it to the guest as it is already (just before
    // text mode started), so it goes the usual way.
    const bool forwarded = hasNative && native.forwarded;
    const bool releaseTakenByIme = hasNative && native.releaseTakenByIme;
    const HostImeKey key = {HostImePath::Raw, event.key(),
                            event.nativeScanCode(), event.nativeVirtualKey()};

    auto held = mHostImeHeld.find(id);
    if (newPress || held == mHostImeHeld.end()) {
        mHostImeHeld.erase(id);
        held = mHostImeHeld.end();
        if (mHostImeTextMode && !forwarded) {
#ifdef _WIN32
            // Qt 6 does not report keys the input method takes for composing
            // (VK_PROCESSKEY); if it ever does, they are the input method's.
            if (event.nativeVirtualKey() == VK_PROCESSKEY) {
                mHostImeHeld[id] = {HostImePath::Ime, key.key,
                                    key.nativeScanCode, key.nativeVirtualKey};
                return true;
            }
#endif
            if (hostImeSendKey(event)) {
                if (!releaseTakenByIme) {
                    mHostImeHeld[id] = {HostImePath::Ime, key.key,
                                        key.nativeScanCode,
                                        key.nativeVirtualKey};
                }
                return true;
            }
        }
        held = mHostImeHeld.emplace(id, key).first;
    } else if (held->second.path != HostImePath::Raw) {
        if (held->second.path == HostImePath::Ime && mHostImeTextMode &&
            !forwarded) {
            hostImeSendKey(event);
        }
        if (releaseTakenByIme) {
            mHostImeHeld.erase(held);
        }
        return true;
    }

    // The usual way.
    if (releaseTakenByIme) {
        // Qt drops the release the input method took: release it right
        // after the press.
        mHostImeHeld.erase(held);
        mHostImeReleaseAfterPress = true;
    }
    if (hasNative && native.deferred) {
        hostImeForwardNativeKey(native.forwardScancode);
    }
    return false;
}

bool EmulatorQtWindow::hostImeKeyRelease(const QKeyEvent& event) {
    auto held = mHostImeHeld.find(hostImeKeyId(event));
    if (held == mHostImeHeld.end()) {
        return false;
    }
    const bool raw = held->second.path == HostImePath::Raw;
    if (!event.isAutoRepeat()) {
        mHostImeHeld.erase(held);
    }
    return !raw;
}

bool EmulatorQtWindow::hostImeKeyIsDown(const HostImeKey& key) const {
#ifdef _WIN32
    return mHostImeNativeDown.count(key.nativeScanCode) > 0;
#else
    return true;
#endif
}

bool EmulatorQtWindow::hostImeDefersNativeKey(quint32 nativeScancode) const {
    if (mHostImeTextMode) {
        return true;
    }
    auto held = mHostImeHeld.find(nativeScancode);
    return held != mHostImeHeld.end() && held->second.path != HostImePath::Raw;
}

// Releases a key held down the usual way, as its release event would.
void EmulatorQtWindow::hostImeReleaseRawKey(const HostImeKey& key) {
    QKeyEvent release(QEvent::KeyRelease, key.key,
                      QGuiApplication::queryKeyboardModifiers(),
                      key.nativeScanCode, key.nativeVirtualKey, 0);
    handleKeyEvent(kEventKeyUp, release);
}

void EmulatorQtWindow::hostImeNativeKeyUp(quint32 nativeScancode,
                                          bool takenByIme) {
    mHostImeNativeDown.erase(nativeScancode);
    if (!takenByIme) {
        return;
    }
    // If key events of this key are still on their way, the last one
    // carries the release; else a key that went the usual way is released
    // now.
    hostImeExpireNativeKeys();
    for (auto it = mHostImeNativeKeys.rbegin(); it != mHostImeNativeKeys.rend();
         ++it) {
        if (it->nativeScancode == nativeScancode) {
            it->releaseTakenByIme = true;
            return;
        }
    }
    auto held = mHostImeHeld.find(nativeScancode);
    if (held != mHostImeHeld.end()) {
        const HostImeKey key = held->second;
        mHostImeHeld.erase(held);
        if (key.path == HostImePath::Raw) {
            hostImeReleaseRawKey(key);
        }
    }
}

// Sends |event| to the guest input method if text mode takes it; returns
// false for keys that go the usual way.
bool EmulatorQtWindow::hostImeSendKey(const QKeyEvent& event) {
    const Qt::KeyboardModifiers mods = event.modifiers();
    const bool shift = mods & Qt::ShiftModifier;
    const bool ctrl = mods & Qt::ControlModifier;
    const bool alt = mods & Qt::AltModifier;
    if (mods & Qt::MetaModifier) {
        return false;
    }

    // Printable text, AltGr (Ctrl+Alt on Windows) included. Ctrl or Alt
    // alone make shortcuts, not text.
    const QString text = event.text();
    if (!text.isEmpty() && ctrl == alt &&
        std::all_of(text.begin(), text.end(), [](QChar c) {
            return c.unicode() >= 0x20 && c.unicode() != 0x7f;
        })) {
        return ModavdImePipe::sendCommit(text.toStdString());
    }

    // Editing keys, and Ctrl(+Shift)+letter. A dead key followed by Enter,
    // Tab or Backspace gives the dead key's text with that key's press, then
    // a press Qt makes up for the key's own character, with key() 0.
    uint32_t keycode = hostImeEditingKeycode(event.key());
    if (!keycode && event.key() == 0 && text.size() == 1) {
        const ushort c = text.at(0).unicode();
        keycode = c == '\r' ? hostImeEditingKeycode(Qt::Key_Return)
                  : c == '\t' ? hostImeEditingKeycode(Qt::Key_Tab)
                  : c == '\b' ? hostImeEditingKeycode(Qt::Key_Backspace)
                              : 0;
    }
    if (keycode) {
        // The emulator's own shortcuts on these keys (Ctrl+Backspace,
        // Ctrl+arrows) keep working.
        if (!mForwardShortcutsToDevice && mToolWindow->isShortcutKey(event)) {
            return false;
        }
    } else if (ctrl && !alt) {
        int letter = event.key();
        if (letter < Qt::Key_A || letter > Qt::Key_Z) {
            // Layouts without Latin letters: the virtual key still is one.
            const quint32 vk = event.nativeVirtualKey();
            letter = (vk >= 'A' && vk <= 'Z') ? int(Qt::Key_A + (vk - 'A')) : 0;
        }
        if (letter) {
            keycode = kAndroidKeycodeA + uint32_t(letter - Qt::Key_A);
        }
    }
    if (!keycode) {
        return false;
    }
    uint32_t metaState = 0;
    if (shift) {
        metaState |= kAndroidMetaShiftOn;
    }
    if (ctrl) {
        metaState |= kAndroidMetaCtrlOn;
    }
    if (alt) {
        metaState |= kAndroidMetaAltOn;
    }
    return ModavdImePipe::sendKey(keycode, metaState,
                                  ModavdImePipe::kKeyDownUp);
}

void EmulatorQtWindow::hostImeNativeKeyDown(quint32 nativeScancode,
                                           bool newPress,
                                           bool modifier,
                                           quint32 forwardScancode,
                                           bool forwarded,
                                           bool deferred) {
    mHostImeNativeDown.insert(nativeScancode);
    if (modifier && !newPress) {
        return;  // Qt reports no auto-repeats of modifiers
    }
    hostImeExpireNativeKeys();
    if (mHostImeNativeKeys.size() >= kHostImeMaxNativeKeys) {
        mHostImeNativeKeys.pop_front();
    }
    mHostImeNativeKeys.push_back({nativeScancode, forwardScancode,
                                  hostImeNowMs(), newPress, forwarded,
                                  deferred, false});
}

void EmulatorQtWindow::hostImeExpireNativeKeys() {
    const qint64 now = hostImeNowMs();
    while (!mHostImeNativeKeys.empty() &&
           now - mHostImeNativeKeys.front().timeMs > kHostImeNativeKeyMs) {
        mHostImeNativeKeys.pop_front();
    }
}

// Takes the native event filter's record of |event|'s WM_KEYDOWN, if any.
bool EmulatorQtWindow::hostImeTakeNativeKey(const QKeyEvent& event,
                                            HostImeNativeKey* key) {
    hostImeExpireNativeKeys();
    for (auto it = mHostImeNativeKeys.begin(); it != mHostImeNativeKeys.end();
         ++it) {
        if (it->nativeScancode == event.nativeScanCode()) {
            *key = *it;
            mHostImeNativeKeys.erase(it);
            return true;
        }
    }
    return false;
}

// Sends a WM_KEYDOWN the native event filter left to the key event, the way
// the filter forwards keys.
void EmulatorQtWindow::hostImeForwardNativeKey(quint32 scancode) {
    if (!getConsoleAgents()->settings->use_keycode_forwarding()) {
        return;
    }
    NativeKeyboardEventHandler::KeyEvent keyEv;
    keyEv.scancode = static_cast<int>(scancode);
    keyEv.modifiers = 0;
    keyEv.eventType = kEventKeyDown;
    std::optional<SkinEvent> maybeEvent =
            NativeKeyboardEventHandler::getInstance()->handleKeyEvent(keyEv);
    if (maybeEvent.has_value()) {
        queueSkinEvent(std::move(maybeEvent.value()));
    }
}

void EmulatorQtWindow::handleInputMethodEvent(QInputMethodEvent* event) {
    event->accept();
    if (!mHostImeTextMode || mHostImeResetting) {
        return;
    }
    const QString commit = event->commitString();
    const QString preedit = event->preeditString();

    if (event->replacementStart() != 0 || event->replacementLength() != 0) {
        // Without composing text the replacement is relative to the cursor,
        // which deleteSurroundingText() can do when it covers the cursor.
        const int start = event->replacementStart();
        const int length = event->replacementLength();
        if (!mHostImeComposing && start <= 0 && length >= -start) {
            ModavdImePipe::sendDelete(-start, length + start);
        } else {
            dwarning("host IME: replacement %d+%d not applied", start, length);
        }
    }
    if (!commit.isEmpty()) {
        ModavdImePipe::sendCommit(commit.toStdString());
        mHostImeComposing = false;
    }
    if (!preedit.isEmpty()) {
        int cursor = -1;
        for (const QInputMethodEvent::Attribute& a : event->attributes()) {
            if (a.type == QInputMethodEvent::Cursor) {
                cursor = a.start;
                break;
            }
        }
        ModavdImePipe::sendPreedit(cursor, preedit.toStdString());
        mHostImeComposing = true;
    } else if (commit.isEmpty() && mHostImeComposing) {
        ModavdImePipe::sendPreedit(-1, std::string());
        ModavdImePipe::sendFinishComposing();
        mHostImeComposing = false;
    }
}

QVariant EmulatorQtWindow::hostImeQuery(Qt::InputMethodQuery query) const {
    if (!mHostImeTextMode) {
        return QVariant();
    }
    switch (query) {
        case Qt::ImEnabled:
            return true;
        case Qt::ImHints: {
            Qt::InputMethodHints hints = Qt::ImhNone;
            const uint32_t typeClass = mHostImeInputType & kInputTypeMaskClass;
            const uint32_t variation =
                    mHostImeInputType & kInputTypeMaskVariation;
            if ((typeClass == kInputTypeClassText &&
                 (variation == kInputTypeTextVariationPassword ||
                  variation == kInputTypeTextVariationVisiblePassword ||
                  variation == kInputTypeTextVariationWebPassword)) ||
                (typeClass == kInputTypeClassNumber &&
                 variation == kInputTypeNumberVariationPassword)) {
                hints |= Qt::ImhHiddenText | Qt::ImhSensitiveData |
                         Qt::ImhNoPredictiveText;
            }
            if (typeClass == kInputTypeClassNumber) {
                hints |= Qt::ImhPreferNumbers;
            }
            return QVariant(hints.toInt());
        }
        case Qt::ImCursorRectangle:
            return hostImeCursorRect();
        default:
            return QVariant();
    }
}

// The guest's cursor rectangle in the container's coordinates: the inverse of
// the mouse mapping (skin_surface_reverse_map(), then the display's place in
// the layout). Skin rotation turns the layout and the guest together, and
// zoom mode scrolls this widget inside the container.
QRect EmulatorQtWindow::hostImeCursorRect() const {
    // The guest display in this widget's coordinates, and its size in guest
    // pixels.
    QRectF display(rect());
    QSizeF guest;
    if (mBackingSurface && mBackingSurface->bitmap &&
        mBackingSurface->w > 0 && mBackingSurface->h > 0) {
        if (hasSkin() && !mInZoomMode && mDeviceGeometry.isValid()) {
            // A device frame: the display is where the GL subwindow is,
            // which outside zoom mode has the same place in the container
            // as in this widget.
            const auto hw = getConsoleAgents()->settings->hw();
            const bool sideways = mOrientation == SKIN_ROTATION_90 ||
                                  mOrientation == SKIN_ROTATION_270;
            guest = sideways ? QSizeF(hw->hw_lcd_height, hw->hw_lcd_width)
                             : QSizeF(hw->hw_lcd_width, hw->hw_lcd_height);
            display = QRectF(mDeviceGeometry);
        } else {
            // No frame: the display fills the layout, which this widget
            // shows scaled to its size.
            guest = QSizeF(mBackingSurface->bitmap->size());
            display = QRectF(0, 0, mBackingSurface->w, mBackingSurface->h);
        }
    }
    const int lineHeight = mContainer.fontMetrics().height();
    // Without a cursor from the guest: the display's bottom left corner.
    QRect area(qRound(display.left()), qRound(display.bottom()) - lineHeight, 1,
               lineHeight);
    if (mHostImeHasCursor && !guest.isEmpty()) {
        const double sx = display.width() / guest.width();
        const double sy = display.height() / guest.height();
        auto x = [&](int v) {
            return display.left() + qBound(0.0, double(v), guest.width()) * sx;
        };
        auto y = [&](int v) {
            return display.top() + qBound(0.0, double(v), guest.height()) * sy;
        };
        // left()/top()/right()/bottom() hold the guest values as sent.
        const QRect& c = mHostImeGuestCursor;
        area = QRectF(QPointF(x(std::min(c.left(), c.right())),
                              y(std::min(c.top(), c.bottom()))),
                      QPointF(x(std::max(c.left(), c.right())),
                              y(std::max(c.top(), c.bottom()))))
                       .toAlignedRect();
        area.setWidth(std::max(area.width(), 1));
        area.setHeight(std::max(area.height(), 1));
    }
    return QRect(mapTo(&mContainer, area.topLeft()), area.size());
}

void EmulatorQtWindow::hostImeUpdateCursor() {
    if (mHostImeTextMode) {
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
    }
}

void EmulatorQtWindow::simulateKeyPress(int keyCode, int modifiers) {
    SkinEvent event = createSkinEvent(kEventKeyDown);
    event.u.key.keycode = keyCode;
    event.u.key.mod = modifiers;
    queueSkinEvent(std::move(event));

    event = createSkinEvent(kEventKeyUp);
    event.u.key.keycode = keyCode;
    event.u.key.mod = modifiers;
    queueSkinEvent(std::move(event));
}

void EmulatorQtWindow::simulateScrollBarChanged(int x, int y) {
    SkinEvent event = createSkinEvent(kEventScrollBarChanged);
    event.u.scroll.x = x;
    event.u.scroll.xmax = mContainer.horizontalScrollBar()->maximum();
    event.u.scroll.y = y;
    event.u.scroll.ymax = mContainer.verticalScrollBar()->maximum();
    queueSkinEvent(std::move(event));
}

void EmulatorQtWindow::setDisplayRegion(int xOffset,
                                        int yOffset,
                                        int width,
                                        int height) {
    SkinEvent event = createSkinEvent(kEventSetDisplayRegion);
    event.u.display_region.xOffset = xOffset;
    event.u.display_region.yOffset = yOffset;
    event.u.display_region.width = width;
    event.u.display_region.height = height;
    queueSkinEvent(std::move(event));
}

void EmulatorQtWindow::setDisplayRegionAndUpdate(int xOffset,
                                                 int yOffset,
                                                 int width,
                                                 int height) {
    SkinEvent event = createSkinEvent(kEventSetDisplayRegionAndUpdate);
    event.u.display_region.xOffset = xOffset;
    event.u.display_region.yOffset = yOffset;
    event.u.display_region.width = width;
    event.u.display_region.height = height;
    queueSkinEvent(std::move(event));
}

void EmulatorQtWindow::simulateSetScale(double scale) {
    // Avoid zoom and scale events clobbering each other if the user rapidly
    // changes zoom levels
    if (mInZoomMode && mNextIsZoom) {
        return;
    }

    // Reset our local copy of zoom factor
    mZoomFactor = 1.0;

    SkinEvent event = createSkinEvent(kEventSetScale);
    event.u.window.scale = scale;
    queueSkinEvent(std::move(event));
}

void EmulatorQtWindow::simulateSetZoom(double zoom) {
    // Avoid zoom and scale events clobbering each other if the user rapidly
    // changes zoom levels
    if (mNextIsZoom || mZoomFactor == zoom) {
        return;
    }

    // Qt Widgets do not get properly sized unless they appear at least once.
    // The scroll bars *must* be properly sized in order for zoom to create the
    // correct GLES subwindow, so this ensures they will be. This is reset as
    // soon as the window is shown.
    mContainer.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    mContainer.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    mNextIsZoom = true;
    mZoomFactor = zoom;

    QSize viewport = mContainer.viewportSize();

    SkinEvent event = createSkinEvent(kEventSetZoom);
    event.u.window.x = viewport.width();
    event.u.window.y = viewport.height();

    QScrollBar* horizontal = mContainer.horizontalScrollBar();
    event.u.window.scroll_h =
            horizontal->isVisible() ? horizontal->height() : 0;
    event.u.window.scale = zoom;
    queueSkinEvent(std::move(event));
}

void EmulatorQtWindow::simulateWindowMoved(const QPoint& pos) {
    SkinEvent event = createSkinEvent(kEventWindowMoved);
    event.u.window.x = pos.x();
    event.u.window.y = pos.y();
    queueSkinEvent(std::move(event));

    mOverlay.move(mContainer.mapToGlobal(QPoint()));
}

void EmulatorQtWindow::simulateZoomedWindowResized(const QSize& size) {
    mOverlay.resize(size);
    if (!mInZoomMode) {
        return;
    }

    SkinEvent event = createSkinEvent(kEventZoomedWindowResized);
    QScrollBar* horizontal = mContainer.horizontalScrollBar();
    event.u.scroll.x = horizontal->value();
    event.u.scroll.y = mContainer.verticalScrollBar()->value();
    event.u.scroll.xmax = size.width();
    event.u.scroll.ymax = size.height();
    event.u.scroll.scroll_h =
            horizontal->isVisible() ? horizontal->height() : 0;
    queueSkinEvent(std::move(event));
}

void EmulatorQtWindow::setForwardShortcutsToDevice(int index) {
    mForwardShortcutsToDevice = (index != 0);
}

void EmulatorQtWindow::slot_runOnUiThread(RunOnUiThreadFunc f,
                                          QSemaphore* semaphore) {
    QSemaphoreReleaser semReleaser(semaphore);
    f();
}

bool EmulatorQtWindow::hasSkin() const {
    char *skinName = nullptr, *skinDir = nullptr;
    avdInfo_getSkinInfo(getConsoleAgents()->settings->avdInfo(), &skinName,
                        &skinDir);
    bool hasSkin = (skinDir != NULL);
    if (mSkinRemoved) {
        hasSkin = false;
    }
    if (skinName) {
        free(skinName);
    }
    if (skinDir) {
        free(skinDir);
    }
    return hasSkin;
}

bool EmulatorQtWindow::hasFrame() const {
    if (ToolWindow::inWindowNav()) {
        return true;
    }
    if (mFrameAlways || mInZoomMode) {
        return true;
    }
    bool hasFrame = !hasSkin();
    return hasFrame;
}

bool EmulatorQtWindow::isInZoomMode() const {
    return mInZoomMode;
}

ToolWindow* EmulatorQtWindow::toolWindow() const {
    return mToolWindow;
}

CarClusterWindow* EmulatorQtWindow::carClusterWindow() const {
    return mCarClusterWindow;
}

CarClusterConnector* EmulatorQtWindow::carClusterConnector() const {
    return mCarClusterConnector;
}

EmulatorContainer* EmulatorQtWindow::containerWindow() {
    return &mContainer;
}

QSize EmulatorQtWindow::guestAreaSize() const {
    QSize size = mContainer.size();
    if (ToolWindow::inWindowNav()) {
        size.rwidth() -= 54;
    }
    return size;
}

void EmulatorQtWindow::showZoomIfNotUserHidden() {
    if (!mOverlay.wasZoomUserHidden()) {
        mOverlay.showForZoom();
    }
}

QSize EmulatorQtWindow::containerSize() const {
    return mContainer.size();
}

QRect EmulatorQtWindow::deviceGeometry() const {
    return mDeviceGeometry;
}

android::emulation::AdbInterface* EmulatorQtWindow::getAdbInterface() const {
    return (*mAdbInterface);
}

void EmulatorQtWindow::toggleZoomMode() {
    mInZoomMode = !mInZoomMode;

    // Exiting zoom mode snaps back to aspect ratio
    if (!mInZoomMode) {
        // Scroll bars should be turned off immediately.
        mContainer.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        mContainer.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        doResize(guestAreaSize());
        mOverlay.hide();
    } else {
        doResize(guestAreaSize());
        mOverlay.showForZoom();
    }
    maskWindowFrame();
}

void EmulatorQtWindow::recenterFocusPoint() {
    mContainer.horizontalScrollBar()->setValue(mFocus.x() * width() -
                                               mViewportFocus.x());
    mContainer.verticalScrollBar()->setValue(mFocus.y() * height() -
                                             mViewportFocus.y());

    mFocus = QPointF();
    mViewportFocus = QPoint();
}

void EmulatorQtWindow::saveZoomPoints(const QPoint& focus,
                                      const QPoint& viewportFocus) {
    // The underlying frame will change sizes, so get what "percentage" of the
    // frame was clicked, where (0,0) is the top-left corner and (1,1) is the
    // bottom right corner.
    mFocus = QPointF((float)focus.x() / this->width(),
                     (float)focus.y() / this->height());

    // Save to re-align the container with the underlying frame.
    mViewportFocus = viewportFocus;
}

void EmulatorQtWindow::scaleDown() {
    doResize(guestAreaSize() / 1.1, true);
}

void EmulatorQtWindow::scaleUp() {
    doResize(guestAreaSize() * 1.1, true);
}

void EmulatorQtWindow::fixScale() {
    doResize(guestAreaSize(), true);
}

void EmulatorQtWindow::zoomIn() {
    const QSize area = ToolWindow::inWindowNav() ? mContainer.viewportSize()
                                                 : mContainer.size();
    zoomIn(QPoint(width() / 2, height() / 2),
           QPoint(area.width() / 2, area.height() / 2));
}

void EmulatorQtWindow::zoomIn(const QPoint& focus,
                              const QPoint& viewportFocus) {
    if (!mBackingSurface)
        return;

    saveZoomPoints(focus, viewportFocus);

    // The below scale = x creates a skin equivalent to calling "window
    // scale x" through the emulator console. At scale = 1, the device
    // should be at a 1:1 pixel mapping with the monitor. We allow going
    // to twice this size.
    double scale = ((double)size().width() /
                    (double)mBackingSurface->bitmap->size().width());
    double maxZoom = mZoomFactor * 2.0 / scale;

    if (scale < 2) {
        simulateSetZoom(std::min(mZoomFactor + .25, maxZoom));
    }
}

void EmulatorQtWindow::zoomOut() {
    const QSize area = ToolWindow::inWindowNav() ? mContainer.viewportSize()
                                                  : mContainer.size();
    zoomOut(QPoint(width() / 2, height() / 2),
            QPoint(area.width() / 2, area.height() / 2));
}

void EmulatorQtWindow::zoomOut(const QPoint& focus,
                               const QPoint& viewportFocus) {
    saveZoomPoints(focus, viewportFocus);
    if (mZoomFactor > 1) {
        simulateSetZoom(std::max(mZoomFactor - .25, 1.0));
    }
}

void EmulatorQtWindow::zoomReset() {
    simulateSetZoom(1);
}

void EmulatorQtWindow::zoomTo(const QPoint& focus, const QSize& rectSize) {
    if (!mBackingSurface)
        return;

    const QSize area = ToolWindow::inWindowNav() ? mContainer.viewportSize()
                                                  : mContainer.size();
    saveZoomPoints(focus, QPoint(area.width() / 2, area.height() / 2));

    // The below scale = x creates a skin equivalent to calling "window
    // scale x" through the emulator console. At scale = 1, the device
    // should be at a 1:1 pixel mapping with the monitor. We allow going to
    // twice this size.
    double scale = ((double)size().width() /
                    (double)mBackingSurface->bitmap->size().width());

    // Calculate the "ideal" zoom factor, which would perfectly frame this
    // rectangle, and the "maximum" zoom factor, which makes scale = 1, and
    // pick the smaller one. Adding 20 accounts for the scroll bars
    // potentially cutting off parts of the selection
    double maxZoom = mZoomFactor * 2.0 / scale;
    const QSize zoomArea = ToolWindow::inWindowNav() ? guestAreaSize()
                                                      : mContainer.size();
    double idealWidthZoom = mZoomFactor * (double)zoomArea.width() /
                            (double)(rectSize.width() + 20);
    double idealHeightZoom = mZoomFactor * (double)zoomArea.height() /
                             (double)(rectSize.height() + 20);

    simulateSetZoom(std::min({idealWidthZoom, idealHeightZoom, maxZoom}));
}

void EmulatorQtWindow::panHorizontal(bool left) {
    QScrollBar* bar = mContainer.horizontalScrollBar();
    if (left) {
        bar->setValue(bar->value() - bar->singleStep());
    } else {
        bar->setValue(bar->value() + bar->singleStep());
    }
}

void EmulatorQtWindow::panVertical(bool up) {
    QScrollBar* bar = mContainer.verticalScrollBar();
    if (up) {
        bar->setValue(bar->value() - bar->singleStep());
    } else {
        bar->setValue(bar->value() + bar->singleStep());
    }
}

bool EmulatorQtWindow::mouseInside() {
    QPoint widget_cursor_coords = mapFromGlobal(QCursor::pos());
    return widget_cursor_coords.x() >= 0 &&
           widget_cursor_coords.x() < width() &&
           widget_cursor_coords.y() >= 0 && widget_cursor_coords.y() < height();
}

template <typename N>
struct QuadraticMotion {
    int length;
    N a;
    N b;
    N c;

    N get(int x) const { return (a * x * x + b * x + c) / (length * length); }

    // Fits f(x) = ax^2 + bx + c, so that it satisfies the following boundary
    // conditions.
    // f(0) = y0
    // f(length) = y1
    // df/dx(length) = 2x + b = 0
    static QuadraticMotion smoothEnd(int length, N y0, N y1) {
        return {
                length,
                y0 - y1,
                -2 * length * y0 + 2 * length * y1,
                length * length * y0,
        };
    }
};

class SwipeGesture {
    constexpr static int kWheelCount = 20;
    typedef QPointF P;
    typedef decltype(P().y()) N;

    QuadraticMotion<N> mMotion;
    P mTouchDownPoint;
    int mTick{0};

public:
    SwipeGesture(const P& touchDownPoint, int delta) {
        mTouchDownPoint = touchDownPoint;
        mMotion = QuadraticMotion<N>::smoothEnd(kWheelCount, 0, delta);
    }

    void tick() {
        if (!atEnd()) {
            mTick++;
        }
    }

    // Starts the motion from the current touch point to the previous
    // destination point shifted by delta.
    void moveMore(int delta) {
        auto newStartPoint = mMotion.get(mTick);
        mTick = 0;
        mMotion = QuadraticMotion<N>::smoothEnd(
                kWheelCount, newStartPoint,
                mMotion.get(mMotion.length) + delta);
    }

    // Starts the motion from the new touch down point with restoring the
    // previous remaining delta + adding new delta.
    void reposition(const P& touchDownPoint, int delta) {
        auto newDelta =
                mMotion.get(mMotion.length) - mMotion.get(mTick) + delta;
        mTick = 0;
        mTouchDownPoint = touchDownPoint;
        mMotion = QuadraticMotion<N>::smoothEnd(kWheelCount, 0, newDelta);
    }

    P point() const {
        return {mTouchDownPoint.x(), mTouchDownPoint.y() + swiped()};
    }

    bool atEnd() const { return mTick == mMotion.length; }

    N swiped() const { return mMotion.get(mTick); }
};

void EmulatorQtWindow::wheelEvent(QWheelEvent* event) {
    if (mIgnoreWheelEvent) {
        event->ignore();
        return;
    }

    if (mBackingSurface == nullptr) {
        return;
    }

    const bool inputDeviceHasWheel =
            android::featurecontrol::isEnabled(
                    android::featurecontrol::VirtioMouse) ||
            android::featurecontrol::isEnabled(
                    android::featurecontrol::VirtioTablet) ||
            android::featurecontrol::isEnabled(
                    android::featurecontrol::VirtioDualModeMouse);

    const QAndroidGlobalVarsAgent* settings = getConsoleAgents()->settings;
    const bool inputDeviceHasRotary =
            android::featurecontrol::isEnabled(
                    android::featurecontrol::VirtioInput) &&
            (settings->hw()->hw_rotaryInput ||
             (settings->avdInfo() &&
              avdInfo_getAvdFlavor(settings->avdInfo()) == AVD_WEAR));

    if (inputDeviceHasWheel) {
        // Mouse is active only when it's grabbed. Tablet and dual-mode mouse
        // are always active.
        const bool inputDeviceActive =
                (android::featurecontrol::isEnabled(
                         android::featurecontrol::VirtioMouse) &&
                 mMouseGrabbed) ||
                android::featurecontrol::isEnabled(
                        android::featurecontrol::VirtioTablet) ||
                android::featurecontrol::isEnabled(
                        android::featurecontrol::VirtioDualModeMouse);
        if (!inputDeviceActive) {
            return;
        }
        const int kScrollAggregationAmount =
                (abs(event->angleDelta().x()) >= 120 ||
                 abs(event->angleDelta().y()) >= 120)
                        ? 120
                        : 15;
        if (android::featurecontrol::isEnabled(
                    android::featurecontrol::VirtioDualModeMouse)) {
            mScrollOverflowXInDegrees += event->angleDelta().x();
            if (abs(mScrollOverflowXInDegrees) >= kScrollAggregationAmount) {
                int scrollDelta =
                        mScrollOverflowXInDegrees / kScrollAggregationAmount;
                handleMouseWheelEvent(scrollDelta, Qt::Orientation::Horizontal);
                mScrollOverflowXInDegrees =
                        mScrollOverflowXInDegrees % kScrollAggregationAmount;
            }

            mScrollOverflowYInDegrees += event->angleDelta().y();
            if (abs(mScrollOverflowYInDegrees) >= kScrollAggregationAmount) {
                int scrollDelta =
                        mScrollOverflowYInDegrees / kScrollAggregationAmount;
                handleMouseWheelEvent(scrollDelta, Qt::Orientation::Vertical);
                mScrollOverflowYInDegrees =
                        mScrollOverflowYInDegrees % kScrollAggregationAmount;
            }
            mTrackpadAggregateTimer.start();
        } else {
            // For most mice, 1 wheel click = 15 degrees
            int scrollScale = 120;
            // Avoid too sensitive wheel events for Desktop.
            if (avdInfo_isDesktopApi36OrHigher(
                getConsoleAgents()->settings->avdInfo())) {
                scrollScale = 1;
            }
            handleMouseWheelEvent(event->angleDelta().y() * scrollScale / 15,
                                  Qt::Orientation::Vertical);
            handleMouseWheelEvent(event->angleDelta().x() * scrollScale / 15,
                                  Qt::Orientation::Horizontal);
        }
    } else if (inputDeviceHasRotary) {
        SkinEvent skin_event = createSkinEvent(kEventRotaryInput);
        skin_event.u.rotary_input.delta = event->angleDelta().y() / 8;
        queueSkinEvent(std::move(skin_event));
    } else {
        // For most mice, 1 wheel click = 15 degrees
        const int delta = event->angleDelta().y() * 120 / 15;
        const auto pos = event->position();
        // Scroll 1/9 of window height per a wheel click.
        const int scaledDelta = mBackingSurface->h * delta / 120 / 9;

        if (!mSwipeGesture) {
            // Start the gesture if it's not yet.
            mSwipeGesture = std::make_unique<SwipeGesture>(pos, scaledDelta);
            mWheelScrollTimer.start();
            handleMouseEvent(kEventMouseButtonDown, kMouseButtonLeft,
                             mSwipeGesture->point(), {0, 0});
        } else if (abs(mSwipeGesture->swiped()) >= mBackingSurface->h / 16) {
            // Reposition the gesture to avoid the fake touch point from going
            // outside of the window.
            auto lastGesturePoint = mSwipeGesture->point();
            mSwipeGesture->reposition(pos, scaledDelta);
            handleMouseEvent(kEventMouseButtonUp, kMouseButtonLeft,
                             lastGesturePoint, {0, 0});
            handleMouseEvent(kEventMouseButtonDown, kMouseButtonLeft,
                             mSwipeGesture->point(), {0, 0});
        } else {
            mSwipeGesture->moveMore(scaledDelta);
        }
    }
}

void EmulatorQtWindow::wheelScrollTimeout() {
    mSwipeGesture->tick();

    handleMouseEvent(kEventMouseMotion, kMouseButtonLeft,
                     mSwipeGesture->point(), {0, 0});
    if (!mSwipeGesture->atEnd()) {
        return;
    }

    mWheelScrollTimer.stop();
    std::unique_ptr<SwipeGesture> gesture(std::move(mSwipeGesture));
    handleMouseEvent(kEventMouseButtonUp, kMouseButtonLeft, gesture->point(),
                     {0, 0});
}

void EmulatorQtWindow::trackpadAggregateTimeout() {
    if (android::featurecontrol::isEnabled(
                android::featurecontrol::VirtioDualModeMouse)) {
        // Sending out a scrolling of 960 so it will be scaled to 1 scroll click
        // unit in android-qemu2-glue/qemu-user-event-agent-impl.c
        if (mScrollOverflowXInDegrees > 0) {
            handleMouseWheelEvent(960, Qt::Orientation::Horizontal);
        } else if (mScrollOverflowXInDegrees < 0) {
            handleMouseWheelEvent(-960, Qt::Orientation::Horizontal);
        }
        if (mScrollOverflowYInDegrees > 0) {
            handleMouseWheelEvent(960, Qt::Orientation::Vertical);
        } else if (mScrollOverflowYInDegrees < 0) {
            handleMouseWheelEvent(-960, Qt::Orientation::Vertical);
        }
        mTrackpadAggregateTimer.stop();
        mScrollOverflowXInDegrees = 0;
        mScrollOverflowYInDegrees = 0;
    }
    return;
}

void EmulatorQtWindow::checkAdbVersionAndWarn() {
    // Do not check for ADB in Fuchsia mode.
    if (getConsoleAgents()->settings->is_fuchsia())
        return;

    QSettings settings;
    if (!(*mAdbInterface)->isAdbVersionCurrent() &&
        settings.value(Ui::Settings::AUTO_FIND_ADB, true).toBool()) {
        std::string adb_path = (*mAdbInterface)->detectedAdbPath();
        if (adb_path.empty()) {
            mAdbWarningBox->setText(
                    tr("Could not automatically detect an ADB binary. Some "
                       "emulator functionality will not work until a custom "
                       "path to ADB  is added. "
                       "This can be done in Extended Controls (...) > Settings "
                       "> General tab > 'Use detected ADB location'"));
        } else {
            mAdbWarningBox->setText(
                    tr("The ADB binary found at %1 is obsolete and has serious "
                       "performance problems with the Android Emulator. Please "
                       "update to a newer version to get significantly faster "
                       "app/file transfer.")
                            .arg(adb_path.c_str()));
        }
        QSettings settings;
        if (settings.value(Ui::Settings::SHOW_ADB_WARNING, true).toBool()) {
            QObject::connect(mAdbWarningBox.ptr(),
                             SIGNAL(buttonClicked(QAbstractButton*)), this,
                             SLOT(slot_adbWarningMessageAccepted()));

            QCheckBox* checkbox = new QCheckBox(tr("Never show this again."));
            checkbox->setCheckState(Qt::Unchecked);
            mAdbWarningBox->setWindowModality(Qt::NonModal);
            mAdbWarningBox->setCheckBox(checkbox);
            mAdbWarningBox->show();
        }
    }
}

void EmulatorQtWindow::slot_adbWarningMessageAccepted() {
    QCheckBox* checkbox = mAdbWarningBox->checkBox();
    if (checkbox->checkState() == Qt::Checked) {
        QSettings settings;
        settings.setValue(Ui::Settings::SHOW_ADB_WARNING, false);
    }
}

bool EmulatorQtWindow::safeShutdownInProgress() const {
    // Forced counts too: a close request in the seconds before QEMU quits
    // must not fall through to the upstream close path.
    return mSafeShutdownState != SafeShutdownState::Idle;
}

// The private adb server may have been restarted since this instance
// registered with it at boot (adb kill-server, a crash). With local transport
// scanning off, as the manager and AUTO-MAS run it, the server then does not
// know the instance until the guest's adbd reconnects, and the shutdown and
// sync commands would fail with "device not found". Register again, the way
// the instance does at boot. A server that is not running yet is started by
// the first adb command, so keep trying for a few seconds.
static void registerWithAdbServer() {
    const int adbPort = getConsoleAgents()->settings->android_adb_port();
    const int clientPort = android::emulation::AdbHostServer::getClientPort();
    if (adbPort <= 0 || clientPort <= 0) {
        return;
    }
    android::base::async([adbPort, clientPort] {
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (android::emulation::AdbHostServer::notify(adbPort,
                                                         clientPort)) {
                return;
            }
            System::get()->sleepMs(1000);
        }
    });
}

void EmulatorQtWindow::startSafeShutdown() {
    if (mSafeShutdownState != SafeShutdownState::Idle) {
        return;
    }

    mSafeShutdownState = SafeShutdownState::StoppingGuest;
    {
        AutoLock lock(safeShutdownLock());
        sSafeShutdownWindow = this;
    }
    dinfo("Safe toolbar shutdown: asking Android to power off");
    System::get()->waitAndKillSelf(25);
    updateWindowTitle();
    mSafeShutdownTimeout.start();
    auto* adb = *mAdbInterface;
    if (!adb) {
        dwarning("Safe toolbar shutdown has no ADB interface; syncing guest");
        startSafeShutdownSync();
        return;
    }

    registerWithAdbServer();
    auto command = adb->runAdbCommand(
            {"wait-for-device", "shell", "su", "0", "setprop", "sys.powerctl",
             "shutdown"},
            [this](const android::emulation::OptionalAdbCommandResult& result) {
                const bool success = result && result->exit_code == 0;
                AutoLock lock(safeShutdownLock());
                if (sSafeShutdownWindow != this) {
                    return;
                }
                QMetaObject::invokeMethod(
                        this,
                        [this, success] {
                            if (mSafeShutdownState !=
                                SafeShutdownState::StoppingGuest) {
                                return;
                            }
                            if (!success) {
                                dwarning("Guest shutdown command failed; syncing guest");
                                startSafeShutdownSync();
                                return;
                            }

                            dinfo("Guest shutdown command accepted; waiting for Android to power off");
                        },
                        Qt::QueuedConnection);
            },
            15000);
    if (!command) {
        dwarning("Safe toolbar shutdown command could not start; syncing guest");
        startSafeShutdownSync();
    }
}

void EmulatorQtWindow::startSafeShutdownSync() {
    if (mSafeShutdownState == SafeShutdownState::Syncing ||
        mSafeShutdownState == SafeShutdownState::Forced) {
        return;
    }

    mSafeShutdownState = SafeShutdownState::Syncing;
    mSafeShutdownTimeout.stop();
    auto* adb = *mAdbInterface;
    if (!adb) {
        dwarning("Safe toolbar shutdown could not run guest sync");
        forceCloseNow();
        return;
    }

    registerWithAdbServer();
    auto command = adb->runAdbCommand(
            {"wait-for-device", "shell", "sync"},
            [this](const android::emulation::OptionalAdbCommandResult& result) {
                const bool success = result && result->exit_code == 0;
                AutoLock lock(safeShutdownLock());
                if (sSafeShutdownWindow != this) {
                    return;
                }
                QMetaObject::invokeMethod(
                        this,
                        [this, success] {
                            if (mSafeShutdownState != SafeShutdownState::Syncing) {
                                return;
                            }
                            if (success) {
                                dinfo("Safe toolbar shutdown: guest synced; quitting");
                            } else {
                                dwarning("Safe toolbar shutdown guest sync failed; forcing exit");
                            }
                            forceCloseNow();
                        },
                        Qt::QueuedConnection);
            },
            // The window is gone and the 25 s guard is still far off: give a
            // sync with a lot of dirty data time to finish.
            5000);
    if (!command) {
        dwarning("Safe toolbar shutdown guest sync could not start; forcing exit");
        forceCloseNow();
    }
}

void EmulatorQtWindow::hideForSafeShutdown() {
    // The windows go away at once; Android keeps shutting down behind them
    // and the process exits on its own (or after the bounded sync).
    mHiddenForSafeShutdown = true;
    mContainer.hideModalOverlay();
    if (mToolWindow) {
        mToolWindow->hide();
        mToolWindow->closeExtendedWindow();
    }
    if (mCarClusterWindow) {
        mCarClusterWindow->hide();
    }
    for (auto const& iter : mMultiDisplayWindow) {
        if (iter.second != nullptr)
            iter.second->hide();
    }
    mContainer.hide();
    mOverlay.hide();
}

void EmulatorQtWindow::forceCloseNow() {
    if (mSafeShutdownState == SafeShutdownState::Forced) {
        return;
    }
    mSafeShutdownState = SafeShutdownState::Forced;
    dwarning("Safe toolbar shutdown: quitting without waiting for Android to power off");
    updateWindowTitle();
    mSafeShutdownTimeout.stop();
    queueQuitEvent();
    // Long enough for QEMU to stop the VM and close its disks after the quit.
    System::get()->waitAndKillSelf(10);
}

void EmulatorQtWindow::runAdbShellPowerDownAndQuit() {
    // we need to run it only once, so don't ever reset this
    if (mStartedAdbStopProcess) {
        return;
    }
    mStartedAdbStopProcess = true;
    (*mAdbInterface)
            ->runAdbCommand(
                    {"shell", "stop"},
                    [this](const android::emulation::
                                   OptionalAdbCommandResult&) {
                        queueQuitEvent();
                    },
                    5000);  // for qemu1, reboot -p will shutdown guest but
                            // hangs, allow 5s
}

void EmulatorQtWindow::checkNestedAndWarn() {
    char hv_vendor_id[16] = {};
    android_get_x86_cpuid_vmhost_vendor_id(hv_vendor_id, sizeof(hv_vendor_id));
    auto vmhost = android_get_x86_cpuid_vendor_vmhost_type(hv_vendor_id);
    // Emulator running on native HW
    if (vmhost == VENDOR_VM_NOTVM) {
        return;
    }
    // Emulator running with software emulation
    AndroidCpuAccelerator accelerator = androidCpuAcceleration_getAccelerator();
    if (accelerator == ANDROID_CPU_ACCELERATOR_NONE) {
        return;
    }
    /* Emulator running on hyper-v hypervisor
     * Typically, when hypervisor is detected, emulator will run nested
     * virtualization. However, for type-1 hypervisor, detecting hypervisor
     * could only mean we are on an "admin" guest (or host as is usually
     * called). In these cases, emulator may actually create a "non-admin"
     * guest and run with it. This is NOT nested virtualization.
     *
     * TODO: Xen??
     */
    if (vmhost == VENDOR_VM_HYPERV &&
        accelerator == ANDROID_CPU_ACCELERATOR_WHPX) {
        return;
    }
    if (getConsoleAgents()
                ->settings->android_cmdLineOptions()
                ->no_nested_warnings) {
        return;
    }
    QSettings settings;
    if (settings.value(Ui::Settings::SHOW_NESTED_WARNING, true).toBool()) {
        QObject::connect(mNestedWarningBox.ptr(),
                         SIGNAL(buttonClicked(QAbstractButton*)), this,
                         SLOT(slot_nestedWarningMessageAccepted()));

        QCheckBox* checkbox = new QCheckBox(tr("Never show this again."));
        checkbox->setCheckState(Qt::Unchecked);
        mNestedWarningBox->setWindowModality(Qt::NonModal);
        mNestedWarningBox->setCheckBox(checkbox);
        mNestedWarningBox->show();
    }
}

void EmulatorQtWindow::displayCheckWarnings() {
    // Do not check for AVD configurations in Fuchsia mode.
    if (getConsoleAgents()->settings->is_fuchsia())
        return;

    QSettings settings;

    auto avd = getConsoleAgents()->settings->avdInfo();
    auto opts = getConsoleAgents()->settings->android_cmdLineOptions();
    auto name = avdInfo_getName(avd);

    auto key =
            absl::StrCat(Ui::Settings::SHOW_COMPATIBILITY_WARNING, "_", name);
    bool showWarnings = settings.value(key, true).toBool();

    auto results = AvdCompatibilityManager::instance().check(avd);
    auto warningString =
            AvdCompatibilityManager::instance().constructIssueString(
                    results, AvdCompatibility::Warning);

    if (!opts->no_window && showWarnings && !warningString.empty()) {
        QMessageBox* nestedGeneralWarningBox = new QMessageBox(
                QMessageBox::Information, tr("Compatibility Warnings"),
                QString::fromStdString(warningString), QMessageBox::Ok, this);
        LOG(INFO) << "Displaying warning message dialog with " << warningString
                  << " to user.";
        QCheckBox* checkbox =
                new QCheckBox(QString::fromStdString(absl::StrFormat(
                        "Never show warnings for avd `%s` again.", name)));
        checkbox->setCheckState(Qt::Unchecked);
        nestedGeneralWarningBox->setWindowModality(Qt::NonModal);
        nestedGeneralWarningBox->setCheckBox(checkbox);
        QAbstractButton* okButton =
                nestedGeneralWarningBox->button(QMessageBox::Ok);
        QObject::connect(okButton, &QAbstractButton::clicked,
                         [key, checkbox, nestedGeneralWarningBox]() {
                             if (checkbox->checkState() == Qt::Checked) {
                                 QSettings settings;
                                 settings.setValue(key, false);
                             }
                             nestedGeneralWarningBox->deleteLater();
                             checkbox->deleteLater();
                         });
        nestedGeneralWarningBox->show();
    }
}

void EmulatorQtWindow::slot_nestedWarningMessageAccepted() {
    QCheckBox* checkbox = mNestedWarningBox->checkBox();
    if (checkbox->checkState() == Qt::Checked) {
        QSettings settings;
        settings.setValue(Ui::Settings::SHOW_NESTED_WARNING, false);
    }
}

void EmulatorQtWindow::rotateSkin(SkinRotation rot) {
    // Hack. Notify the parent container that we're rotating, so it doesn't
    // start a regular scaling timer: we know that the scale is correct as
    // it was correct before the rotation.
    mOrientation = rot;
    mScreenMaskChanged = true;
    mContainer.prepareForRotation();

    {
        SkinEvent event = createSkinEvent(kEventLayoutRotate);
        event.u.layout_rotation.rotation = rot;
        queueSkinEvent(std::move(event));
    }

    const bool not_pixel_fold = !android_foldable_is_pixel_fold();
    if (not_pixel_fold && android_foldable_is_folded()) {
        resizeAndChangeAspectRatio(true);
    }

    if (resizableEnabled() && !resizableEnabled34()) {
        PresetEmulatorSizeInfo info;
        if (getResizableConfig(getResizableActiveConfigId(), &info)) {
            resizeAndChangeAspectRatio(0, 0, info.width, info.height);
        }
    }

    if (not_pixel_fold &&
        mToolWindow->getUiEmuAgent()->multiDisplay->isMultiDisplayEnabled()) {
        {
            uint32_t w = 0;
            uint32_t h = 0;
            mToolWindow->getUiEmuAgent()->multiDisplay->getCombinedDisplaySize(
                    &w, &h);
            resizeAndChangeAspectRatio(0, 0, w, h);
        }

        mToolWindow->getUiEmuAgent()->multiDisplay->performRotation(rot);

        {
            // note: this is the initial w and h, not rotated at all
            uint32_t w = 0;
            uint32_t h = 0;
            mToolWindow->getUiEmuAgent()->multiDisplay->getCombinedDisplaySize(
                    &w, &h);
            resizeAndChangeAspectRatio(0, 0, w, h, true);
        }
    }

#ifdef __APPLE__
    {
        // To fix issues when resizing + linking against macos sdk 11.
        queueSkinEvent(createSkinEventScreenChanged());
    }
#endif
}

void EmulatorQtWindow::setVisibleExtent(QBitmap bitMap) {
    QImage bitImage = bitMap.toImage();
    int topVisible = -1;
    for (int row = 0; row < bitImage.height() && topVisible < 0; row++) {
        for (int col = 0; col < bitImage.width(); col++) {
            if (bitImage.pixelColor(col, row) != Qt::color0) {
                topVisible = row;
                break;
            }
        }
    }
    int bottomVisible = -1;
    for (int row = bitImage.height() - 1; row >= 0 && bottomVisible < 0;
         row--) {
        for (int col = 0; col < bitImage.width(); col++) {
            if (bitImage.pixelColor(col, row) != Qt::color0) {
                bottomVisible = row;
                break;
            }
        }
    }

    int leftVisible = -1;
    for (int col = 0; col < bitImage.width() && leftVisible < 0; col++) {
        for (int row = 0; row < bitImage.height(); row++) {
            if (bitImage.pixelColor(col, row) != Qt::color0) {
                leftVisible = col;
                break;
            }
        }
    }
    int rightVisible = -1;
    for (int col = bitImage.width() - 1; col >= 0 && rightVisible < 0; col--) {
        for (int row = 0; row < bitImage.height(); row++) {
            if (bitImage.pixelColor(col, row) != Qt::color0) {
                rightVisible = col;
                break;
            }
        }
    }

    if (topVisible < 0 || rightVisible < 0 || bottomVisible < 0 ||
        leftVisible < 0) {
        mSkinGapTop = 0;
        mSkinGapRight = 0;
        mSkinGapBottom = 0;
        mSkinGapLeft = 0;
    } else {
        mSkinGapTop = topVisible;
        mSkinGapRight = bitImage.width() - 1 - rightVisible;
        mSkinGapBottom = bitImage.height() - 1 - bottomVisible;
        mSkinGapLeft = leftVisible;
    }
}

void EmulatorQtWindow::updateUIMultiDisplayPage(uint32_t id) {
    // update the MutiDisplay UI page, e.g., config through console,
    // loaded from snapshot
    if (id > 0 && id <= MultiDisplayPage::sMaxItem) {
        emit(updateMultiDisplayPage(id));
    }
}

bool EmulatorQtWindow::getMultiDisplay(uint32_t id,
                                       int32_t* x,
                                       int32_t* y,
                                       uint32_t* w,
                                       uint32_t* h,
                                       uint32_t* dpi,
                                       uint32_t* flag,
                                       bool* enabled) {
    const auto uiAgent = mToolWindow->getUiEmuAgent();
    return (uiAgent != nullptr && uiAgent->multiDisplay != nullptr &&
            uiAgent->multiDisplay->getMultiDisplay(id, x, y, w, h, dpi, flag,
                                                   enabled));
}

int EmulatorQtWindow::switchMultiDisplay(bool enabled,
                                         uint32_t id,
                                         int32_t x,
                                         int32_t y,
                                         uint32_t width,
                                         uint32_t height,
                                         uint32_t dpi,
                                         uint32_t flag) {
    const auto uiAgent = mToolWindow->getUiEmuAgent();
    return (uiAgent != nullptr && uiAgent->multiDisplay != nullptr &&
            uiAgent->multiDisplay->setMultiDisplay(id, x, y, width, height, dpi,
                                                   flag, enabled));
}

bool EmulatorQtWindow::getMonitorRect(uint32_t* width, uint32_t* height) {
    SkinRect monitor;
    skin_winsys_get_monitor_rect(&monitor);
    if (width) {
        *width = monitor.size.w;
    }
    if (height) {
        *height = monitor.size.h;
    }
    return true;
}

void EmulatorQtWindow::setFoldedSkin() {
    if (hasSkin()) {
        avdInfo_setCurrentSkin(getConsoleAgents()->settings->avdInfo(),
                               "closed");
        ScreenMask::loadMask();
        mScreenMaskPixmap = QPixmap::fromImage(ScreenMask::getMaskImage());
        mScreenMaskChanged = true;
        EmulatorSkin::getInstance()->reset();
    }
    runOnUiThread(
            [this]() { skin_event_add(createSkinEvent(kEventSetFoldedSkin)); });
}

void EmulatorQtWindow::setNoSkin() {
    if (android_foldable_is_pixel_fold()) {
        return;
    }
    runOnUiThread([this]() {
        char *skinName, *skinDir;
        avdInfo_getSkinInfo(getConsoleAgents()->settings->avdInfo(), &skinName,
                            &skinDir);

        if (skinDir != NULL) {
            skin_event_add(createSkinEvent(kEventSetNoSkin));
            mSkinRemoved = true;
        }
        AFREE(skinName);
        AFREE(skinDir);
        setFrameAlways(true);
    });
}

void EmulatorQtWindow::restoreSkin() {
    if (android_foldable_is_pixel_fold() && hasSkin()) {
        avdInfo_setCurrentSkin(getConsoleAgents()->settings->avdInfo(),
                               "default");
        EmulatorSkin::getInstance()->reset();
        ScreenMask::loadMask();
        mScreenMaskPixmap = QPixmap::fromImage(ScreenMask::getMaskImage());
        mScreenMaskChanged = true;
    }
    runOnUiThread([this]() {
        char *skinName, *skinDir;
        avdInfo_getSkinInfo(getConsoleAgents()->settings->avdInfo(), &skinName,
                            &skinDir);

        // always restore for pixel fold, as the skin w and h
        // changed
        const bool pixel_fold = android_foldable_is_pixel_fold();
        if (pixel_fold || skinDir != NULL) {
            skin_event_add(createSkinEvent(kEventRestoreSkin));
            mSkinRemoved = false;
        }
        AFREE(skinName);
        AFREE(skinDir);
        mToolWindow->hideRotationButton(false);
        setFrameAlways(false);
    });
}

void EmulatorQtWindow::setUIDisplayRegion(int x,
                                          int y,
                                          int w,
                                          int h,
                                          bool ignoreOrientation) {
    runOnUiThread([this, x, y, w, h, ignoreOrientation]() {
        this->resizeAndChangeAspectRatio(x, y, w, h, ignoreOrientation);
    });
}

bool EmulatorQtWindow::addMultiDisplayWindow(uint32_t id,
                                             bool add,
                                             uint32_t w,
                                             uint32_t h) {
    SkinEvent skin_event;
    if (add) {
        QRect windowGeo = this->geometry();
        if (!mBackingSurface) {
            LOG(ERROR) << "backing surface not ready, cancel window creation";
            return false;
        }
        skin_event = createSkinEvent(kEventAddDisplay);
        skin_event.u.add_display.width = w;
        skin_event.u.add_display.height = h;
        skin_event.u.add_display.id = id;
        QSize backingSize = mBackingSurface->bitmap->size();
        float scale = (float)windowGeo.width() / (float)backingSize.width();
        if (mMultiDisplayWindow[id] == nullptr) {
            // create qt window for the 1st time.
            mMultiDisplayWindow[id].reset(new MultiDisplayWidget(w, h, id));
            if (mStreamer) {
                mMultiDisplayWindow[id]->initializeStreamer(mTransportType);
            }
            char title[16];
            uint32_t tId;
            if (id >= android::MultiDisplay::s_displayIdInternalBegin &&
                id < android::MultiDisplay::s_maxNumMultiDisplay) {
                // displayIds created by rcCommands
                tId = id - android::MultiDisplay::s_displayIdInternalBegin + 1;
            } else {
                tId = id;
            }
            LOG(INFO) << "DisplayId: " << id << ", Title: " << tId;
            snprintf(title, 16, "Display %d", tId);
            mMultiDisplayWindow[id]->setWindowTitle(title);
            QRect geoTool = ToolWindow::inWindowNav()
                                     ? mContainer.frameGeometry()
                                     : mToolWindow->geometry();
            mMultiDisplayWindow[id]->move(
                    geoTool.right() + (mMultiDisplayWindow.size() - 1) * 100,
                    geoTool.top());
        }
        mMultiDisplayWindow[id]->setMouseTracking(true);
        mMultiDisplayWindow[id]->setFixedSize(
                QSize((int)(w * scale), (int)(h * scale)));
        mMultiDisplayWindow[id]->show();
    } else {
        auto iter = mMultiDisplayWindow.find(id);
        if (iter == mMultiDisplayWindow.end()) {
            return false;
        }
        mMultiDisplayWindow.erase(iter);
        skin_event = createSkinEvent(kEventRemoveDisplay);
        skin_event.u.remove_display.id = id;
    }
    queueSkinEvent(std::move(skin_event));
    return true;
}

bool EmulatorQtWindow::paintMultiDisplayWindow(uint32_t id, uint32_t texture) {
    if (mMultiDisplayWindow[id] == nullptr ||
        mMultiDisplayWindow[id]->isHidden()) {
        return true;
    }
    mMultiDisplayWindow[id]->paintWindow(texture);
    return true;
}

bool EmulatorQtWindow::multiDisplayParamValidate(uint32_t id,
                                                 uint32_t w,
                                                 uint32_t h,
                                                 uint32_t dpi,
                                                 uint32_t flag) {
    const auto uiAgent = mToolWindow->getUiEmuAgent();
    return uiAgent->multiDisplay->multiDisplayParamValidate(id, w, h, dpi,
                                                            flag);
}

void EmulatorQtWindow::saveMultidisplayToConfig() {
    int pos_x, pos_y;
    uint32_t width, height, dpi, flag;
    const int maxEntries = avdInfo_maxMultiDisplayEntries();
    for (uint32_t i = 1; i < maxEntries + 1; i++) {
        if (!getMultiDisplay(i, &pos_x, &pos_y, &width, &height, &dpi, &flag,
                             nullptr)) {
            avdInfo_replaceMultiDisplayInConfigIni(
                    getConsoleAgents()->settings->avdInfo(), i, -1, -1, 0, 0, 0,
                    0);
        } else {
            avdInfo_replaceMultiDisplayInConfigIni(
                    getConsoleAgents()->settings->avdInfo(), i, pos_x, pos_y,
                    width, height, dpi, flag);
        }
    }
}

/* Convert from tilt coordinates to rotation of pen
 * xTilt: the plane angle (in degrees, range [-60,60]) between
 * the Y-Z plane and the plane containing both the pen and the Y axis
 *      positive values are towards the screen's physical right
 * yTilt: the plane angle (in degrees, range [-60,60]) between
 * the X-Z plane and the plane containing both the pen and the X axis
 *      positive values are towards the bottom of the screen (user)
 * rotation: the azimuth angle (in degrees, range (-180,180]) of the pen,
 * where 0 represents a pen whose cap is pointing down (tip points up),
 * +90 to the left, -90 to the right, 180 cap points up, clockwise.
 * When the pen is perpendicular to the screen the rotation is 0. */
int EmulatorQtWindow::tiltToRotation(int xTiltDeg, int yTiltDeg) {
    int rotationDeg = 0;
    // The equations used, in an X-Y-Z coordinate system:
    // tan(X Tilt) = X / Z
    // tan(Y Tilt) = Y / Z
    // tan(azimuth) = Y / X = tan(Y Tilt) / tan(X Tilt)
    // azimuth = arctan( tan(Y Tilt) / tan(X Tilt) )

    if ((xTiltDeg != 0) && (yTiltDeg != 0)) {
        // domain error may occur if both arguments x,y of atan2 are 0

        // pen is not parallel to any axis, nor perpendicular to screen
        // use the south-clockwise convention for azimuth: (-X, -Y)
        // the Y axis is already reversed on a screen (down, not up)
        double xTiltRad = -qDegreesToRadians((double)xTiltDeg);
        double yTiltRad = qDegreesToRadians((double)yTiltDeg);

        // atan2 function computes the arc tangent of y/x using the signs
        // of arguments to determine the correct quadrant
        rotationDeg = (int)qRadiansToDegrees(
                std::atan2(std::tan(xTiltRad), std::tan(yTiltRad)));
    } else if (xTiltDeg != 0) {
        // pen is parallel to the X axis, not perpendicular to screen
        rotationDeg = (xTiltDeg > 0) ? -90 : 90;
    } else {
        // pen is parallel to the Y axis, or perpendicular to screen
        rotationDeg = (yTiltDeg >= 0) ? 0 : 180;
    }

    return rotationDeg;
}

// Adapt the rotation angle depending on screen rotation
int EmulatorQtWindow::penOrientation(int rotation) {
    int orientation = rotation;

    switch (mOrientation) {
        case SKIN_ROTATION_0:
            break;
        case SKIN_ROTATION_90:
            orientation -= 90;
            break;
        case SKIN_ROTATION_180:
            orientation -= 180;
            break;
        case SKIN_ROTATION_270:
            orientation -= 270;
            break;
        default:
            break;
    }
    // keep the orientation in the range (-180, 180] degrees
    if (orientation <= -180) {
        orientation += 360;
    }

    return orientation;
}

// State machine that translates the pen event types based on button states
// If during a touching state the button is pressed this generates unwanted
// Press and Release events which are translated to Move events
SkinEventType EmulatorQtWindow::translatePenEventType(
        SkinEventType type,
        Qt::MouseButton button,
        Qt::MouseButtons buttons) {
    SkinEventType newType = type;

    switch (mPenTouchState) {
        case TouchState::NOT_TOUCHING:
            // Only the first Press event can have the same pressed buttons
            // button:  only the button that caused the event
            // buttons: all the pressed buttons
            if ((type == kEventPenPress) && (button == buttons)) {
                mPenTouchState = TouchState::TOUCHING;
                newType = kEventPenPress;
            } else {
                newType = kEventPenRelease;
            }
            break;
        case TouchState::TOUCHING:
            // Only the last Release event can have no pressed buttons
            if ((type == kEventPenRelease) && (buttons == Qt::NoButton)) {
                mPenTouchState = TouchState::NOT_TOUCHING;
                newType = kEventPenRelease;
            } else {
                newType = kEventPenMove;
            }
            break;
        default:
            mPenTouchState = TouchState::NOT_TOUCHING;
            newType = kEventPenRelease;
            break;
    }

    return newType;
}

// State machine that translates the mouse event types based on button states
// If during a touching state the button is pressed this generates unwanted
// Press and Release events which are translated to Move events
SkinEventType EmulatorQtWindow::translateMouseEventType(
        SkinEventType type,
        Qt::MouseButton button,
        Qt::MouseButtons buttons) {
    if (android::featurecontrol::isEnabled(
                android::featurecontrol::VirtioMouse)) {
        // If mouse input is intended to be just a mouse, process the event
        // normally.
        return type;
    }

    SkinEventType newType = type;

    switch (mMouseTouchState) {
        case TouchState::NOT_TOUCHING:
            // Only the first Press event can have the same pressed buttons
            // button:  only the button that caused the event
            // buttons: all the pressed buttons
            if ((type == kEventMouseButtonDown) && (button == buttons)) {
                mMouseTouchState = TouchState::TOUCHING;
                newType = kEventMouseButtonDown;
            } else if (android::featurecontrol::isEnabled(
                               android::featurecontrol::VirtioTablet)) {
                newType = kEventMouseMotion;
            } else {
                newType = kEventMouseButtonUp;
            }
            break;
        case TouchState::TOUCHING:
            // Only the last Release event can have no pressed buttons
            if ((type == kEventMouseButtonUp) && (buttons == Qt::NoButton)) {
                mMouseTouchState = TouchState::NOT_TOUCHING;
                newType = kEventMouseButtonUp;
            } else {
                newType = kEventMouseMotion;
            }
            break;
        default:
            mMouseTouchState = TouchState::NOT_TOUCHING;
            newType = kEventMouseButtonUp;
            break;
    }

    return newType;
}

void EmulatorQtWindow::updateScreenMask() {
    if (mScreenMaskPixmap.isNull()) {
        mRotatedScreenMaskPixmap = QPixmap();
        mScreenMaskChanged = false;
        return;
    }

    QTransform rotater;
    int rotationAmount = 0;
    switch (mOrientation) {
        case SKIN_ROTATION_0:
            rotationAmount = 0;
            break;
        case SKIN_ROTATION_90:
            rotationAmount = 90;
            break;
        case SKIN_ROTATION_180:
            rotationAmount = 180;
            break;
        case SKIN_ROTATION_270:
            rotationAmount = 270;
            break;
        default:
            rotationAmount = 0;
            break;
    }
    rotater.rotate(rotationAmount);
    mRotatedScreenMaskPixmap = mScreenMaskPixmap.transformed(rotater);
    mScreenMaskChanged = false;
}
