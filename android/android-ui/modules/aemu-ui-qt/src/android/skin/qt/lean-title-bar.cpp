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
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QSize>

#include <algorithm>

namespace {

// The palette of the lean window, the same as its navigation buttons had.
constexpr char kStyle[] =
        "#leanTitleBar { background: #20212D; }"
        "#leanTitleBar QLabel { color: #E8E8EC; background: transparent; }"
        "#leanTitleBar QPushButton { background: #20212D; border: 0; }"
        "#leanTitleBar QPushButton:hover { background: #303142; }"
        "#leanTitleBar QPushButton:pressed { background: #3B3D52; }"
        "#leanTitleBar QPushButton:disabled { background: #20212D; }"
        "#leanTitleBarDivider { background: #3B3D52; border: 0; }";

constexpr int kButtonWidth = 40;
constexpr int kNavIconSize = 20;
constexpr int kWindowIconSize = 16;

}  // namespace

LeanTitleBar::LeanTitleBar(QWidget* window) : QWidget(window), mWindow(window) {
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

    mBack = addButton("back", QString::fromUtf8("返回"), kNavIconSize);
    mHome = addButton("home", QString::fromUtf8("桌面"), kNavIconSize);
    mDivider = new QFrame(this);
    mDivider->setObjectName(QStringLiteral("leanTitleBarDivider"));
    mDivider->setFixedSize(1, 16);
    mMinimize = addButton("minimize", QString::fromUtf8("最小化"), kWindowIconSize);
    mClose = addButton("close", QString::fromUtf8("关机"), kWindowIconSize);

    layout->addWidget(mBack);
    layout->addWidget(mHome);
    layout->addSpacing(4);
    layout->addWidget(mDivider, 0, Qt::AlignVCenter);
    layout->addSpacing(4);
    layout->addWidget(mMinimize);
    layout->addWidget(mClose);

    mWindow->installEventFilter(this);
    updateTitle();
    updateIcon();
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
    for (QWidget* widget : {static_cast<QWidget*>(mBack),
                            static_cast<QWidget*>(mHome),
                            static_cast<QWidget*>(mDivider)}) {
        if (!widget->isHidden()) {
            return widget->x();
        }
    }
    return mMinimize->x();
}

void LeanTitleBar::setButtonsEnabled(bool enabled) {
    for (QPushButton* button : {mBack, mHome, mMinimize, mClose}) {
        button->setEnabled(enabled);
    }
}

bool LeanTitleBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == mWindow) {
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
