// Copyright (C) 2016 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/skin/qt/emulator-container.h"

#include <qframe.h>       // for QFrame::NoFrame
#include <qglobal.h>      // for Q_ASSERT
#include <qlist.h>        // for QList<>::iterator
#include <qnamespace.h>   // for WindowMinimized
#include <qwindowdefs.h>  // for WId
#include <QFlags>         // for QFlags
#include <QByteArray>      // for qgetenv
#include <QFrame>         // for QFrame
#include <QMoveEvent>     // for QMoveEvent
#include <QObject>        // for QObject
#include <QPoint>         // for QPoint
#include <QScrollBar>     // for QScrollBar
#include <QStyle>         // for QStyle
#include <QStyleFactory>  // for QStyleFactory
#include <QStyleHints>
#include <QTimer>
#include <QVector2D>      // for QVector2D
#include <algorithm>      // for min, find, max
#include <functional>     // for __base
#include <tuple>          // for tuple
#include <utility>        // for move

#include "android/cmdline-option.h"                // for android_cmdLineOptions
#include "android/skin/qt/ModalOverlay.h"          // for ModalOverlay
#include "android/skin/qt/OverlayMessageCenter.h"  // for OverlayMessageCe...
#include "android/console.h"
#include "android/avd/info.h"
#include "android/skin/qt/GlassesStatusOverlay.h"
#include "android/skin/qt/VirtualSceneInfoDialog.h"  // for VirtualSceneInfo...
#include "android/skin/qt/emulator-qt-window.h"      // for EmulatorQtWindow
#include "android/skin/qt/size-tweaker.h"            // for SizeTweaker
#include "android/skin/qt/tool-window.h"             // for ToolWindow
#include "android/utils/debug.h"                     // for VERBOSE_PRINT
#include "android/utils/file_data.h"
#include "android/utils/property_file.h"

class QCloseEvent;
class QFocusEvent;
class QKeyEvent;
class QMoveEvent;
class QResizeEvent;
class QScrollBar;
class QShowEvent;
class QStyle;

#if defined(__APPLE__)
#include "android/skin/qt/mac-native-window.h"  // for getNSWindow, nsW...
#endif

#if defined(_WIN32)
#include "android/skin/qt/windows-native-window.h"
#endif

static constexpr int kEventBufferSize = 8;

EmulatorContainer::EmulatorContainer(EmulatorQtWindow* window)
    : mEmulatorWindow(window), mMessages(this) {
    mEventBuffer.reserve(kEventBufferSize);
    setFrameShape(QFrame::NoFrame);
    setWidget(window);

    setMinimumSize({200, 200});

    // The following hints prevent the minimize/maximize/close buttons from
    // appearing.
#ifdef __APPLE__
    setWindowFlags(Qt::WindowTitleHint | Qt::Window);
#else
    const bool lean = ToolWindow::inWindowNav();
    setWindowFlags(Qt::WindowTitleHint | Qt::Window | Qt::CustomizeWindowHint |
                   (lean ? (Qt::WindowSystemMenuHint |
                            Qt::WindowMinimizeButtonHint |
                            Qt::WindowCloseButtonHint)
                         : Qt::WindowFlags()));
    if (lean) {
        setStyleSheet(QStringLiteral("EmulatorContainer { background: #000000; }"));
    }
#endif

#ifdef __APPLE__
    // Digging into the Qt source code reveals that if the above flags are set
    // on OSX, the created window will be given a style mask that removes the
    // resize handles from the window. The "Maximize" hint below is the specific
    // customization flag that ensures the window will have resize handles.
    // The "Minimize" hint is needed to get the window to respond to our
    // programmatic minimize operation.
    // We add "Minimize" and "Maximize" now, then immediately disable them when
    // the window is first shown.
    setWindowFlags(this->windowFlags() | Qt::WindowMinimizeButtonHint |
                   Qt::WindowMaximizeButtonHint);

    // On OS X the native scrollbars disappear when not in use which
    // makes the zoomed-in emulator window look unscrollable. Also, due
    // to the semi-transparent nature of the scrollbar, it will
    // interfere with the main GL window, causing all kinds of ugly
    // effects.
    QStyle* style = QStyleFactory::create("Fusion");
    if (style) {
        this->verticalScrollBar()->setStyle(style);
        this->horizontalScrollBar()->setStyle(style);
        QObject::connect(this, &QObject::destroyed, [style] { delete style; });
    }

#endif  // __APPLE__

    mResizeTimer.setSingleShot(true);
    connect(&mResizeTimer, SIGNAL(timeout()), this, SLOT(slot_resizeDone()));

    connect(this, SIGNAL(showModalOverlay(QString)), this,
            SLOT(slot_showModalOverlay(QString)));
    connect(this, SIGNAL(hideModalOverlay()), this,
            SLOT(slot_hideModalOverlay()));
    connect(this,
            SIGNAL(setModalOverlayFunc(QString,
                                       Ui::ModalOverlay::OverlayButtonFunc)),
            this,
            SLOT(slot_setModalOverlayFunc(
                    QString, Ui::ModalOverlay::OverlayButtonFunc)));
    connect(this, SIGNAL(showVirtualSceneInfoDialog()), this,
            SLOT(slot_showVirtualSceneInfoDialog()));
    connect(this, SIGNAL(hideVirtualSceneInfoDialog()), this,
            SLOT(slot_hideVirtualSceneInfoDialog()));
    connect(this, SIGNAL(displayPowerModeChanged(uint32_t, int)), this,
            SLOT(slot_displayPowerModeChanged(uint32_t, int)));

    // Detect AI Glasses and add GlassesStatusOverlay
    if (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) == AVD_GLASSES) {
        mGlassesStatusOverlay = new GlassesStatusOverlay(this);

        // For displayless AI Glasses, show the "no display" status overlay
        QString displayDevice = getBootProp("ro.boot.display_device");
        if (displayDevice == "none") {
            mGlassesStatusOverlay->setStatusNoDisplay();
        } else {
            auto multiDisplayAgent = getConsoleAgents()->multi_display;
            if (multiDisplayAgent &&
                multiDisplayAgent->getDisplayPowerModeEventListener) {
                auto notifier = static_cast<
                        android::DisplayPowerModeNotificationSupport*>(
                        multiDisplayAgent->getDisplayPowerModeEventListener());
                if (notifier) {
                    mDisplayPowerModeEventListener = std::make_unique<
                            android::base::RaiiEventListener<
                                    android::
                                            DisplayPowerModeNotificationSupport,
                                    android::DisplayPowerModeChangeEvent>>(
                            notifier,
                            [this](const android::DisplayPowerModeChangeEvent
                                           evt) {
                                emit this->displayPowerModeChanged(
                                        evt.displayId,
                                        static_cast<int>(evt.powerMode));
                            });
                }
            }

            if (multiDisplayAgent && multiDisplayAgent->getDisplayPowerMode) {
                uint32_t mode = 0;
                if (multiDisplayAgent->getDisplayPowerMode(0, &mode) == 0) {
                    slot_displayPowerModeChanged(0, mode);
                }
            }
        }
    }
}

EmulatorContainer::~EmulatorContainer() {
    slot_hideModalOverlay();
    slot_hideVirtualSceneInfoDialog();

    // This object is owned directly by |window|.  Avoid circular
    // destructor calls by explicitly unsetting the widget.
    takeWidget();

    // Same thing with the parent here.
    mMessages->setParent(nullptr);
}

bool EmulatorContainer::event(QEvent* e) {
    // Ignore MetaCall and UpdateRequest events, and don't snap in zoom mode.
    if (mEmulatorWindow->isInZoomMode() || e->type() == QEvent::MetaCall ||
        e->type() == QEvent::UpdateRequest) {
        return QScrollArea::event(e);
    }

    if (mEventBuffer.size() < kEventBufferSize) {
        mEventBuffer.push_back(e->type());
    } else {
        // Overwrite the first element and move it to the back to avoid extra
        // allocations.
        mEventBuffer.first() = e->type();
        mEventBuffer.move(0, mEventBuffer.size() - 1);
    }

    // Scan to see if a resize event happened recently
    const auto resizeIt =
            std::find(mEventBuffer.begin(), mEventBuffer.end(), QEvent::Resize);
    if (resizeIt != mEventBuffer.end()) {
        // Determining resize-over is OS specific
        // Do so by scanning the remainder of the event buffer for specific
        // combinations

#ifdef _WIN32
        if (std::find(std::next(resizeIt), mEventBuffer.end(),
                      QEvent::NonClientAreaMouseButtonRelease) !=
            mEventBuffer.end()) {
#elif defined(__linux__)
        static constexpr QEvent::Type sequence[] = {QEvent::WindowActivate,
                                                    QEvent::ActivationChange,
                                                    QEvent::FocusIn};
        if (std::search(std::next(resizeIt), mEventBuffer.end(),
                        std::begin(sequence),
                        std::end(sequence)) != mEventBuffer.end()) {
#elif defined(__APPLE__)
        if (e->type() == QEvent::NonClientAreaMouseMove ||
            e->type() == QEvent::Enter || e->type() == QEvent::Leave) {
#endif
            // clear() deallocates internal buffer, but we need it.
            mEventBuffer.erase(mEventBuffer.begin(), mEventBuffer.end());
            mEmulatorWindow->doResize(mEmulatorWindow->guestAreaSize());

            // Kill the resize timer to avoid double resizes.
            stopResizeTimer();
        }
    }

    if (e->type() == QEvent::NonClientAreaMouseButtonRelease) {
        // Tool-window sometimes doesn't resize correctly when moving across displays. As a
        // workaround, we request for a tool-window repaint when the user releases the mouse button
        // after dragging the window.
        mEmulatorWindow->toolWindow()->repaint();
    }

    return QScrollArea::event(e);
}

void EmulatorContainer::changeEvent(QEvent* event) {
    switch (event->type()) {
        case QEvent::WindowStateChange:
            // Strictly preventing the maximizing (called "zooming" on OS X) of
            // a window is hard - it changes by host, and even by window manager
            // on Linux. Therefore, we counteract it by seeing if the window
            // ever enters a maximized state, and if it does, immediately
            // undoing that maximization.
            //
            // Note that we *do not* call event->ignore(). Maximizing happens in
            // the OS-level window, not Qt's representation of the window. This
            // event simply notifies the Qt representation (and us) that the
            // OS-level window has changed to a maximized state. We do not want
            // to ignore this state change, we just want to counteract the
            // effects it had.
            if (windowState() & Qt::WindowMaximized) {
                // BUG: 142088252 On macOS, if the emulator window
                // - coincides with the monitor dimensions
                // - the host's device pixel ratio is 1,
                // it's possible to enter an infinite recursive event loop if
                // we try the functions here.
#ifndef __APPLE__
                showNormal();
                if (mModalOverlay) {
                    mModalOverlay->showNormal();
                }
                if (mVirtualSceneInfo) {
                    mVirtualSceneInfo->showNormal();
                }
                if (mGlassesStatusOverlay) {
                    mGlassesStatusOverlay->showOverlay();
                }
                mMessages->showNormal();
#endif
            } else if (windowState() & Qt::WindowMinimized) {
                // In case the window was minimized without pressing the
                // toolbar's minimize button (which is possible on some
                // window managers), remember to hide the toolbar (which
                // will also hide the extended window, if it exists).
                // showMinimized();
                // bug: 128977914
                // This code is super risky to run because even though
                // the window state returns minimized, we could be
                // in the process of restoring from minimize,
                // which ends up hiding a bunch of stuff again
                // when the emulator restores.
                // Instead, opt for the lesser evil of leaving tool
                // windows and other objects visible.
                // mEmulatorWindow->toolWindow()->hide();
                // if (mModalOverlay) {
                //     mModalOverlay->hide();
                // }
                // if (mVirtualSceneInfo) {
                //     mVirtualSceneInfo->hide();
                // }
                // mMessages->hide();

                // Note that we are hiding the glasses overlay in a different
                // way to bypass this issue.
                if (mGlassesStatusOverlay) {
                    mGlassesStatusOverlay->hideOverlay();
                }
            } else if (windowState() == Qt::WindowNoState) {
                // The window has no state set (in normal state).
                if (mGlassesStatusOverlay) {
                    mGlassesStatusOverlay->showOverlay();
                }
            }

            break;

        case QEvent::ActivationChange:
#if defined(__linux__)
            // b/79158122: When restoring from minimize or a desktop switch,
            // Linux/OpenGL does not refresh the screen on it's own, normal
            // Linux/QT does work.  This logic pushes a refresh event to work
            // around the issue.  Note that putting this logic under
            // QEvent::WindowStateChange above does not work as that event comes
            // too early for refreshes to be effective.
            mEmulatorWindow->queueSkinEvent(createSkinEvent(kEventForceRedraw));
#endif
            break;

        default:
            // Ignore other event types
            break;
    }
}

void EmulatorContainer::closeEvent(QCloseEvent* event) {
    // make sure the toolWindow's close button is the only exit point
    // from qt: we could get closeEvent directly from host windowing
    // framework.
    if (mEmulatorWindow->isMainThreadRunning() &&
        !mEmulatorWindow->toolWindow()->closeButtonClicked()) {
        mEmulatorWindow->toolWindow()->on_close_button_clicked();
        event->ignore();
        return;
    }

    if (mEmulatorWindow->isMainThreadRunning() &&
        mEmulatorWindow->safeShutdownInProgress()) {
        mEmulatorWindow->closeEvent(event);
        return;
    }

    slot_hideModalOverlay();
    slot_hideVirtualSceneInfoDialog();
    mEmulatorWindow->closeEvent(event);
    // In embedded mode, sometimes QCoreApplication::quit() doesn't get called,
    // which means the Qt main loop never quits. So let's explicitly call quit()
    // here prevent that situation from occurring.
    if (getConsoleAgents()
                ->settings->android_cmdLineOptions()
                ->qt_hide_window) {
        qApp->quit();
    }
}

void EmulatorContainer::focusInEvent(QFocusEvent* event) {
    mEmulatorWindow->toolWindow()->raise();
    if (mModalOverlay) {
        mModalOverlay->raise();
    }
    if (mVirtualSceneInfo) {
        mVirtualSceneInfo->raise();
    }
    mMessages.ifExists([this] { mMessages->raise(); });
    if (mEmulatorWindow->isInZoomMode()) {
        mEmulatorWindow->showZoomIfNotUserHidden();
    }
}

void EmulatorContainer::focusOutEvent(QFocusEvent* event) {
    mEmulatorWindow->focusOutEvent(event);
    QFrame::focusOutEvent(event);
}

// The container has the keyboard focus, so it is where the host input method
// sends its events while the window is in host IME text mode.
void EmulatorContainer::inputMethodEvent(QInputMethodEvent* event) {
    if (mEmulatorWindow->isHostImeTextMode()) {
        mEmulatorWindow->handleInputMethodEvent(event);
        return;
    }
    QScrollArea::inputMethodEvent(event);
}

QVariant EmulatorContainer::inputMethodQuery(Qt::InputMethodQuery query) const {
    QVariant value = mEmulatorWindow->hostImeQuery(query);
    return value.isValid() ? value : QScrollArea::inputMethodQuery(query);
}

void EmulatorContainer::keyPressEvent(QKeyEvent* event) {
    mEmulatorWindow->keyPressEvent(event);
}

void EmulatorContainer::keyReleaseEvent(QKeyEvent* event) {
    mEmulatorWindow->keyReleaseEvent(event);
}

void EmulatorContainer::moveEvent(QMoveEvent* event) {
    QScrollArea::moveEvent(event);
    mEmulatorWindow->simulateWindowMoved(event->pos());
    mEmulatorWindow->toolWindow()->dockMainWindow();
    adjustModalOverlayGeometry();
    adjustVirtualSceneDialogGeometry();
    adjustMessagesOverlayGeometry();
    adjustGlassesStatusOverlayGeometry();
}

void EmulatorContainer::resizeEvent(QResizeEvent* event) {
    QScrollArea::resizeEvent(event);
    mEmulatorWindow->simulateZoomedWindowResized(this->viewportSize());
    adjustModalOverlayGeometry();
    adjustVirtualSceneDialogGeometry();
    adjustMessagesOverlayGeometry();
    adjustGlassesStatusOverlayGeometry();

    if (mRotating) {
        // Rotation event also generate a resize, but it shouldn't recalculate
        // the sizes via scaling - scale is already correct, we've rotatated
        // from a correct shape.
        VERBOSE_PRINT(rotation, "Ignored a resize event on rotation");
        mRotating = false;
    } else {
        // To solve some resizing edge cases on OSX/Windows/KDE, start a short
        // timer that will attempt to trigger a resize in case the user's mouse
        // has not entered the window again.
        startResizeTimer();
    }
}

bool EmulatorContainer::viewportEvent(QEvent* event) {
    const bool handled = QScrollArea::viewportEvent(event);
    if (event->type() == QEvent::Resize && ToolWindow::inWindowNav() &&
        mEmulatorWindow->toolWindow()) {
        mEmulatorWindow->toolWindow()->dockMainWindow();
    }
    return handled;
}

void EmulatorContainer::showEvent(QShowEvent* event) {
#ifdef _WIN32
    if (ToolWindow::inWindowNav()) {
        setDarkTitleBar(effectiveWinId());
        if (!mDarkTitleBarConnected) {
            mDarkTitleBarConnected = true;
            QObject::connect(QGuiApplication::styleHints(),
                             &QStyleHints::colorSchemeChanged, this, [this] {
                                 QTimer::singleShot(0, this, [this] {
                                     if (ToolWindow::inWindowNav()) {
                                         setDarkTitleBar(effectiveWinId());
                                     }
                                 });
                             });
        }
    }
#endif
// Disable the minimize and maximize buttons on OSX. See the comment in the
// constructor for an explanation of why this is necessary.
#ifdef __APPLE__
    WId wid = effectiveWinId();
    wid = (WId)getNSWindow((void*)wid);
    nsWindowHideWindowButtons((void*)wid);

    // The following is an OS X specific hack.
    // Explanation:
    // move events aren't delivered to the window widget until
    // AFTER the user has finished dragging the window. This means that the
    // toolbar window will stay in its place as the user drags the main window,
    // and will snap back to it after the drag operation finishes.
    // The behavior we actually want is for the toolbar to follow the main
    // window as it is being dragged (i.e. same as on Win and Linux). The only
    // way to achieve this on OS X is apparently to make the tool window a
    // "child" of the main window (using OS X's native API).
    Q_ASSERT(mEmulatorWindow->toolWindow());
    mEmulatorWindow->toolWindow()
            ->showNormal();  // force creation of native window id
    WId tool_wid = mEmulatorWindow->toolWindow()->effectiveWinId();
    tool_wid = (WId)getNSWindow((void*)tool_wid);
    if (wid && tool_wid) {
        nsWindowAdopt((void*)wid, (void*)tool_wid);
        mEmulatorWindow->toolWindow()->setTouchpadWindowMacParent(wid);
    }
#endif  // __APPLE__

    // showEvent() gets called when the emulator is minimized because we are
    // calling showMinimized(), which *technically* is a show event. We only
    // want to re-show the toolbar when we are transitioning to a not-minimized
    // state. However, we must do it after changing flags on Linux.
    if (!(windowState() & Qt::WindowMinimized)) {
// As seen below in showMinimized(), we need to remove the minimize button on
// Linux when the window is re-shown. We know this show event is from being
// un-minimized because the minimized button flag is present.
#ifdef __linux__
        if (!mEmulatorWindow->isWindowMinimized()) {
            // Changing window flags requires re-showing this window to ensure
            // the flags are appropriately changed.
            showNormal();

            // The subwindow won't redraw until the guest screen changes, which
            // may not happen for a minute (when the clock changes), so force a
            // redraw after re-showing the window.
            mEmulatorWindow->queueSkinEvent(createSkinEvent(kEventForceRedraw));
            mEmulatorWindow->toolWindow()->show();
            mEmulatorWindow->toolWindow()->dockMainWindow();
        }

#else   // __linux__

        mEmulatorWindow->toolWindow()->show();
        mEmulatorWindow->toolWindow()->dockMainWindow();
#endif  // !__linux__
        if (mModalOverlay) {
            mModalOverlay->showNormal();
        }
        if (mVirtualSceneInfo) {
            mVirtualSceneInfo->showNormal();
        }
        if (mGlassesStatusOverlay) {
            mGlassesStatusOverlay->showOverlay();
        }
        mMessages.ifExists([&] { mMessages->showNormal(); });
    }
}

void EmulatorContainer::show() {
    // When emulator is running in embedded mode,
    // hide all windows except for extended controls window.
    if (getConsoleAgents()
                ->settings->android_cmdLineOptions()
                ->qt_hide_window) {
        return;
    }
    QScrollArea::show();
}
void EmulatorContainer::showMinimized() {
// Some Linux window managers (specifically, Compiz, which is the default
// Ubuntu window manager) will not allow minimizing unless the minimize
// button is actually there! So, we re-add the button, minimize the window,
// and then remove the button when it gets reshown.
#ifdef __linux__
    setWindowFlags(windowFlags() | Qt::WindowMinimizeButtonHint);
    showNormal();
#endif  // __linux__
    QScrollArea::showMinimized();
}

void EmulatorContainer::stopResizeTimer() {
    mResizeTimer.stop();
}

QSize EmulatorContainer::viewportSize() const {
    QSize output = this->size();

    if (ToolWindow::inWindowNav()) {
        output.rwidth() -= 54;
    }

    QScrollBar* vertical = this->verticalScrollBar();
    output.setWidth(output.width() -
                    (vertical->isVisible() ? vertical->width() : 0));

    QScrollBar* horizontal = this->horizontalScrollBar();
    output.setHeight(output.height() -
                     (horizontal->isVisible() ? horizontal->height() : 0));

    return output;
}

Ui::OverlayMessageCenter& EmulatorContainer::messageCenter() {
    connect(mMessages.ptr(), SIGNAL(resized()), this,
            SLOT(slot_messagesResized()), Qt::UniqueConnection);
    return mMessages.get();
}

#ifdef __linux__
// X11 doesn't have a getter for current mouse button state. Use the Qt's
// synchronous mouse state tracking that might be a little bit off.
static int numHeldMouseButtons() {
    const auto buttons = QApplication::mouseButtons();
    int numButtons = 0;
    if (buttons & Qt::LeftButton) {
        ++numButtons;
    }
    if (buttons & Qt::RightButton) {
        ++numButtons;
    }
    return numButtons;
}
#endif

void EmulatorContainer::slot_resizeDone() {
    if (mEmulatorWindow->isInZoomMode()) {
        return;
    }

    // Only do a resize if no mouse buttons are held down.
    // A hacky way of determining if the user is still holding down for a
    // resize. This queries the global event state to see if any mouse buttons
    // are held down. If there are, then the user must not be done resizing
    // yet.
    if (numHeldMouseButtons() == 0) {
        mEmulatorWindow->doResize(mEmulatorWindow->guestAreaSize());
    } else {
        startResizeTimer();
    }
}

void EmulatorContainer::slot_showModalOverlay(QString text) {
    slot_hideModalOverlay();
    slot_hideVirtualSceneInfoDialog();
    // When emulator is running in embedded mode,
    // hide all windows except for extended controls window.
    if (getConsoleAgents()
                ->settings->android_cmdLineOptions()
                ->qt_hide_window) {
        return;
    }
    mModalOverlay = new Ui::ModalOverlay(text, this);
    adjustModalOverlayGeometry();
    mModalOverlay->show();

    if (windowState() & Qt::WindowMinimized) {
        mModalOverlay->hide();
    }
}

void EmulatorContainer::slot_hideModalOverlay() {
    if (mModalOverlay) {
        mModalOverlay->hide([](Ui::ModalOverlay* mo) { mo->deleteLater(); });
        mModalOverlay = nullptr;

        if (mShouldCreateVirtualSceneInfo) {
            mShouldCreateVirtualSceneInfo = false;
            slot_showVirtualSceneInfoDialog();
        }
    }
}

void EmulatorContainer::slot_setModalOverlayFunc(
        QString text,
        Ui::ModalOverlay::OverlayButtonFunc f) {
    if (mModalOverlay) {
        mModalOverlay->showButtonFunc(text, std::move(f));
    }
}

void EmulatorContainer::slot_showVirtualSceneInfoDialog() {
    if (mVirtualSceneInfo) {
        return;
    }

    // If there's an existing modal overlay, defer creating the virtual scene
    // info dialog until that is dismissed.
    if (mModalOverlay) {
        mShouldCreateVirtualSceneInfo = true;
    } else {
        mVirtualSceneInfo = new VirtualSceneInfoDialog(this);
        connect(mVirtualSceneInfo, SIGNAL(closeButtonPressed()), this,
                SIGNAL(hideVirtualSceneInfoDialog()));

        adjustVirtualSceneDialogGeometry();

        mVirtualSceneInfo->show();
        if (windowState() & Qt::WindowMinimized) {
            mVirtualSceneInfo->hide();
        }
    }
}

void EmulatorContainer::slot_hideVirtualSceneInfoDialog() {
    if (mVirtualSceneInfo) {
        mVirtualSceneInfo->hide(
                [](VirtualSceneInfoDialog* info) { info->deleteLater(); });
        mVirtualSceneInfo = nullptr;
    }
}

void EmulatorContainer::slot_messagesResized() {
    adjustMessagesOverlayGeometry();
}

void EmulatorContainer::startResizeTimer() {
    // We use a longer timer on Linux because it is not needed on non-KDE
    // systems, so we want it to be less noticeable.
#ifdef __linux__
    mResizeTimer.start(1000);
#else
    mResizeTimer.start(500);
#endif
}

void EmulatorContainer::adjustModalOverlayGeometry() {
    if (!mModalOverlay) {
        return;
    }

    auto overlaySize = QSize(std::min(300, width()), std::min(300, height()));
    mModalOverlay->resize(overlaySize, size());
    mModalOverlay->move(
            mapToGlobal(QPoint((width() - mModalOverlay->width()) / 2,
                               (height() - mModalOverlay->height()) / 2)));
}

void EmulatorContainer::adjustVirtualSceneDialogGeometry() {
    if (!mVirtualSceneInfo) {
        return;
    }

    mVirtualSceneInfo->resize(size());
    mVirtualSceneInfo->move(
            mapToGlobal(QPoint((width() - mVirtualSceneInfo->width()) / 2,
                               (height() - mVirtualSceneInfo->height()) / 2)));
}

void EmulatorContainer::adjustMessagesOverlayGeometry() {
    if (!mMessages.hasInstance()) {
        return;
    }

    auto scaleFactor = SizeTweaker::scaleFactor(this).x();
    auto w = std::min<int>(width() - 2 * 30 * scaleFactor,
                           std::max<int>(300 * scaleFactor,
                                         (width() - 2 * 150 * scaleFactor)));
    mMessages->setFixedWidth(w);
    mMessages->adjustSize();
    mMessages->move(mapToGlobal(QPoint()) +=
                    {(width() - mMessages->width()) / 2, 0});
}

void EmulatorContainer::adjustGlassesStatusOverlayGeometry() {
    if (!mGlassesStatusOverlay) {
        return;
    }

    auto scaleFactor = SizeTweaker::scaleFactor(this).x();
    mGlassesStatusOverlay->move(mapToGlobal(QPoint(10 * scaleFactor, 10 * scaleFactor)));
}

void EmulatorContainer::slot_displayPowerModeChanged(uint32_t displayId,
                                                     int mode) {
    if (displayId != 0 || !mGlassesStatusOverlay) {
        return;
    }
    if (mode == static_cast<int>(android::DisplayPowerMode::OFF)) {
        mGlassesStatusOverlay->setStatusDisplayOff();
    } else {
        mGlassesStatusOverlay->setStatusDisplayOn();
    }
    adjustGlassesStatusOverlayGeometry();
}

QString EmulatorContainer::getBootProp(const char* prop) {
    if (!getConsoleAgents() || !getConsoleAgents()->settings) {
        return "";
    }
    const AvdInfo* avd = getConsoleAgents()->settings->avdInfo();
    if (!avd) {
        return "";
    }
    const FileData* bootProps = avdInfo_getBootProperties(avd);
    if (!bootProps || fileData_isEmpty(bootProps)) {
        return "";
    }

    char* val = propertyFile_getValue((const char*)bootProps->data,
                                      bootProps->size, prop);
    if (!val) {
        return "";
    }
    QString result = QString::fromUtf8(val);
    free(val);
    return result;
}
