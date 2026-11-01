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

#pragma once

#include <qcoreevent.h>                    // for QEvent::Type, QEvent (ptr ...
#include <qobjectdefs.h>                   // for Q_OBJECT, signals, slots
#include <QEvent>                          // for QEvent
#include <QList>                           // for QList
#include <QScrollArea>                     // for QScrollArea
#include <QSize>                           // for QSize
#include <QString>                         // for QString
#include <QTimer>                          // for QTimer

#include <memory>

#include "aemu/base/EventNotificationSupport.h"
#include "aemu/base/memory/OnDemand.h"  // for OnDemand
#include "android/skin/qt/ModalOverlay.h"  // for ModalOverlay, ModalOverlay...
#include "android/skin/qt/OverlayMessageCenter.h"
#include "host-common/MultiDisplay.h"

class EmulatorQtWindow;
class QCloseEvent;
class QFocusEvent;
class QInputMethodEvent;
class QKeyEvent;
class QMoveEvent;
class QObject;
class QResizeEvent;
class QShowEvent;
class QSize;
class QString;
class QWidget;
class VirtualSceneInfoDialog;
class GlassesStatusOverlay;

class EmulatorContainer : public QScrollArea {
    Q_OBJECT

public:
    explicit EmulatorContainer(EmulatorQtWindow* window);
    virtual ~EmulatorContainer();

    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool event(QEvent* e) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool viewportEvent(QEvent* event) override;

    void show();
    void showMinimized();

    void stopResizeTimer();
    QSize viewportSize() const;

    void setLeanToolbarWidth(int width) { setViewportMargins(0, 0, width, 0); }

    void prepareForRotation() { mRotating = true; }

    Ui::OverlayMessageCenter& messageCenter();

signals:
    void showModalOverlay(QString text);
    void hideModalOverlay();
    void setModalOverlayFunc(QString text,
                             Ui::ModalOverlay::OverlayButtonFunc func);
    void showVirtualSceneInfoDialog();
    void hideVirtualSceneInfoDialog();
    void displayPowerModeChanged(uint32_t displayId, int mode);

private slots:
    void slot_resizeDone();
    void slot_showModalOverlay(QString text);
    void slot_hideModalOverlay();
    void slot_setModalOverlayFunc(QString text,
                                  Ui::ModalOverlay::OverlayButtonFunc func);
    void slot_showVirtualSceneInfoDialog();
    void slot_hideVirtualSceneInfoDialog();
    void slot_messagesResized();
    void slot_displayPowerModeChanged(uint32_t displayId, int mode);

private:
    void startResizeTimer();
    void adjustModalOverlayGeometry();
    void adjustVirtualSceneDialogGeometry();
    void adjustMessagesOverlayGeometry();
    void adjustGlassesStatusOverlayGeometry();

    QString getBootProp(const char* prop);

    EmulatorQtWindow* mEmulatorWindow;
    Ui::ModalOverlay* mModalOverlay = nullptr;
    VirtualSceneInfoDialog* mVirtualSceneInfo = nullptr;
    GlassesStatusOverlay* mGlassesStatusOverlay = nullptr;
    bool mShouldCreateVirtualSceneInfo = false;
    bool mVirtualSceneWindowShown = false;
    android::base::MemberOnDemandT<Ui::OverlayMessageCenter, QWidget*>
            mMessages;
    QList<QEvent::Type> mEventBuffer;
    QTimer mResizeTimer;
    bool mRotating = false;
    std::unique_ptr<android::base::RaiiEventListener<
            android::DisplayPowerModeNotificationSupport,
            android::DisplayPowerModeChangeEvent>>
            mDisplayPowerModeEventListener;
};
