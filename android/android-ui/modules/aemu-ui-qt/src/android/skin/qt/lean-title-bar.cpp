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
#include "android/skin/qt/lean-title-bar.h"

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPoint>
#include <QPushButton>
#include <QRect>
#include <QResizeEvent>
#include <QSize>
#include <QStyle>
#include <QToolTip>

#include <algorithm>
#include <utility>

#include "android/lean-title-bar-logic.h"
#include "android/lean-window-frame.h"

namespace {

// The palette of the lean window, the same as its navigation buttons had.
// leanHot is the maximize / restore button's highlight while the window, not
// Qt, sees the mouse over it.
constexpr char kStyle[] =
        "#leanTitleBar { background: #20212D; }"
        "#leanTitleBar QLabel { color: #E8E8EC; background: transparent; }"
        "#leanTitleBar QPushButton { background: #20212D; border: 0; }"
        "#leanTitleBar QPushButton:hover { background: #303142; }"
        "#leanTitleBar QPushButton[leanHot=\"true\"] { background: #303142; }"
        "#leanTitleBar QPushButton:pressed { background: #3B3D52; }"
        "#leanTitleBar QPushButton:disabled { background: #20212D; }"
        "#leanTitleBarDivider { background: #3B3D52; border: 0; }";

constexpr int kButtonWidth = 40;
constexpr int kNavIconSize = 20;
constexpr int kWindowIconSize = 16;

}  // namespace

LeanTitleBar::LeanTitleBar(QWidget* window, bool volumeShown)
    : QWidget(window), mWindow(window) {
    setObjectName(QStringLiteral("leanTitleBar"));
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QString::fromLatin1(kStyle));
    setFocusPolicy(Qt::NoFocus);
    setFixedHeight(kHeight);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 0, 0, 0);
    layout->setSpacing(0);

    mIcon = new QLabel(this);
    mIcon->setFixedSize(kWindowIconSize, kWindowIconSize);
    layout->addWidget(mIcon, 0, Qt::AlignVCenter);
    layout->addSpacing(8);

    mTitle = new QLabel(this);
    mTitle->setMinimumWidth(0);
    mTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    mTitle->setTextFormat(Qt::PlainText);
    layout->addWidget(mTitle, 1, Qt::AlignVCenter);

    // The screenshot and volume icons are drawn at the window buttons' size,
    // as on the side toolbar.
    mScreenshot = addButton("screen_shot", QString::fromUtf8("截图"),
                            kWindowIconSize);
    if (volumeShown) {
        mVolume = addButton("volume_up", QString::fromUtf8("音量"),
                            kWindowIconSize);
    }
    mBack = addButton("back", QString::fromUtf8("返回"), kNavIconSize);
    mHome = addButton("home", QString::fromUtf8("桌面"), kNavIconSize);
    mDivider = new QFrame(this);
    mDivider->setObjectName(QStringLiteral("leanTitleBarDivider"));
    mDivider->setFixedSize(1, 16);
    mMinimize = addButton("minimize", QString::fromUtf8("最小化"), kWindowIconSize);
    mMaximize = addButton("window_maximize", QString::fromUtf8("最大化"),
                          kWindowIconSize);
    mClose = addButton("close", QString::fromUtf8("关机"), kWindowIconSize);

    using android::lean_title_bar::Item;
    for (Item item : android::lean_title_bar::items(volumeShown)) {
        switch (item) {
            case Item::Screenshot:
                layout->addWidget(mScreenshot);
                break;
            case Item::Volume:
                layout->addWidget(mVolume);
                break;
            case Item::Back:
                layout->addWidget(mBack);
                break;
            case Item::Home:
                layout->addWidget(mHome);
                break;
            case Item::Divider:
                layout->addSpacing(4);
                layout->addWidget(mDivider, 0, Qt::AlignVCenter);
                layout->addSpacing(4);
                break;
            case Item::Minimize:
                layout->addWidget(mMinimize);
                break;
            case Item::Maximize:
                layout->addWidget(mMaximize);
                break;
            case Item::Close:
                layout->addWidget(mClose);
                break;
        }
    }

    // The tool tip of the maximize / restore button while the window sees
    // the mouse over it: Qt shows none for a widget it gets no mouse events
    // for.
    mMaximizeTipTimer.setSingleShot(true);
    connect(&mMaximizeTipTimer, &QTimer::timeout, this,
            &LeanTitleBar::showMaximizeToolTip);

    mWindow->installEventFilter(this);
    updateTitle();
    updateIcon();
    updateMaximizeButton();
}

QPushButton* LeanTitleBar::addButton(const char* icon,
                                     const QString& tip,
                                     int iconSize) {
    auto* button = new QPushButton(this);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFlat(true);
    button->setFixedSize(kButtonWidth, kHeight);
    // The dark theme's icons are drawn light.
    button->setIcon(QIcon(QStringLiteral(":/dark/") + QLatin1String(icon)));
    button->setIconSize(QSize(iconSize, iconSize));
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    return button;
}

int LeanTitleBar::buttonsLeft() const {
    // The first button shown; back or home may be hidden for some AVDs.
    for (QWidget* widget : {static_cast<QWidget*>(mScreenshot),
                            static_cast<QWidget*>(mVolume),
                            static_cast<QWidget*>(mBack),
                            static_cast<QWidget*>(mHome),
                            static_cast<QWidget*>(mDivider)}) {
        if (widget && !widget->isHidden()) {
            return widget->x();
        }
    }
    return mMinimize->x();
}

void LeanTitleBar::setButtonsEnabled(bool enabled) {
    mButtonsEnabled = enabled;
    for (QPushButton* button :
         {mScreenshot, mBack, mHome, mMinimize, mMaximize, mClose}) {
        button->setEnabled(enabled);
    }
    if (mVolume) {
        mVolume->setEnabled(enabled && mVolumeAvailable);
    }
    if (!enabled) {
        setMaximizeButtonMouse(false, false);
        emit buttonsDisabled();
    }
}

void LeanTitleBar::setVolumeAvailable(bool available, const QString& reason) {
    mVolumeAvailable = available;
    if (!mVolume) {
        return;
    }
    mVolume->setEnabled(mButtonsEnabled && available);
    const QString tip = available ? QString::fromUtf8("音量") : reason;
    mVolume->setToolTip(tip);
    mVolume->setAccessibleName(tip);
}

void LeanTitleBar::showScreenshotNotice(const QString& text) {
    constexpr int kNoticeMs = 4000;
    QToolTip::showText(
            mScreenshot->mapToGlobal(QPoint(0, mScreenshot->height())), text,
            mScreenshot, QRect(), kNoticeMs);
}

void LeanTitleBar::setMaximizedQuery(std::function<bool()> query) {
    mMaximizedQuery = std::move(query);
    updateMaximizeButton();
}

void LeanTitleBar::updateMaximizeButton() {
    const bool maximized =
            mMaximizedQuery ? mMaximizedQuery() : mWindow->isMaximized();
    const bool restore = android::lean_window::maxButtonFace(maximized) ==
                         android::lean_window::MaxButtonFace::Restore;
    if (restore == mShowsRestore) {
        // addButton() gave it the maximize face.
        return;
    }
    mShowsRestore = restore;
    const QString tip = restore ? QString::fromUtf8("还原")
                                : QString::fromUtf8("最大化");
    // The dark theme's icons are drawn light, as in addButton().
    mMaximize->setIcon(QIcon(restore ? QStringLiteral(":/dark/window_restore")
                                     : QStringLiteral(":/dark/window_maximize")));
    mMaximize->setToolTip(tip);
    // Screen readers and UI Automation hear of the new name.
    mMaximize->setAccessibleName(tip);
    if (QToolTip::isVisible() && mMaximizeHot) {
        showMaximizeToolTip();
    }
}

void LeanTitleBar::setMaximizeButtonMouse(bool hot, bool down) {
    if (!mMaximize->isEnabled()) {
        hot = false;
        down = false;
    }
    if (hot != mMaximizeHot) {
        mMaximizeHot = hot;
        mMaximize->setProperty("leanHot", hot);
        // A style sheet reads a dynamic property only when polished again.
        mMaximize->style()->unpolish(mMaximize);
        mMaximize->style()->polish(mMaximize);
        if (hot) {
            mMaximizeTipTimer.start(
                    style()->styleHint(QStyle::SH_ToolTip_WakeUpDelay));
        } else {
            mMaximizeTipTimer.stop();
            if (QToolTip::isVisible() && QToolTip::text() == mMaximize->toolTip()) {
                QToolTip::hideText();
            }
        }
    }
    if (down != mMaximize->isDown()) {
        mMaximize->setDown(down);
    }
    mMaximize->update();
}

void LeanTitleBar::showMaximizeToolTip() {
    if (mMaximizeHot) {
        QToolTip::showText(QCursor::pos(), mMaximize->toolTip(), mMaximize);
    }
}

bool LeanTitleBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == mWindow) {
        if (event->type() == QEvent::WindowStateChange ||
            event->type() == QEvent::Resize || event->type() == QEvent::Show) {
            // Maximized or restored by the button, a double click on the
            // title bar, Win+Up / Win+Down, a drag to the top of the screen
            // or the system menu (ModAVD#185).
            updateMaximizeButton();
        }
        if (event->type() == QEvent::WindowTitleChange) {
            updateTitle();
        } else if (event->type() == QEvent::WindowIconChange ||
                   event->type() == QEvent::ApplicationWindowIconChange ||
                   event->type() == QEvent::ScreenChangeInternal) {
            // On another screen the icon is drawn for its pixel ratio.
            updateIcon();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void LeanTitleBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateTitle();
}

void LeanTitleBar::updateTitle() {
    const QString title = mWindow->windowTitle();
    const QString shown = mTitle->fontMetrics().elidedText(
            title, Qt::ElideRight, std::max(0, mTitle->width()));
    if (mTitle->text() != shown) {
        mTitle->setText(shown);
    }
    mTitle->setToolTip(shown == title ? QString() : title);
}

void LeanTitleBar::updateIcon() {
    QIcon icon = mWindow->windowIcon();
    if (icon.isNull()) {
        icon = QApplication::windowIcon();
    }
    mIcon->setPixmap(icon.pixmap(QSize(kWindowIconSize, kWindowIconSize),
                                 devicePixelRatioF()));
}
