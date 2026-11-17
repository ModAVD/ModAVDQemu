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
#pragma once

#include <QString>
#include <QTimer>
#include <QWidget>

#include <functional>

class QEvent;
class QFrame;
class QLabel;
class QPushButton;
class QResizeEvent;

// The title bar the lean window draws itself (ModAVD#58), at the top of the
// emulator window: the window's icon and title on the left, then screenshot
// (ModAVD#231), volume while the emulator plays sound (ModAVD#226), back,
// home, a divider, minimize, maximize / restore (ModAVD#185) and close on the
// right (lean_title_bar::items()). It shows the title and icon of the window
// it sits in. Moving the window by the title bar, and the system menu, are
// the window's (WindowsLeanFrame); this only has the buttons.
class LeanTitleBar : public QWidget {
    Q_OBJECT

public:
    // Logical pixels.
    static constexpr int kHeight = 32;

    LeanTitleBar(QWidget* window, bool volumeShown);

    // Where the buttons start, in logical pixels from the left edge.
    int buttonsLeft() const;
    void setButtonsEnabled(bool enabled);

    QPushButton* screenshotButton() const { return mScreenshot; }
    // Null without sound.
    QPushButton* volumeButton() const { return mVolume; }
    // Whether the volume button can be used: without adb it is disabled
    // and its tool tip says |reason|.
    void setVolumeAvailable(bool available, const QString& reason);
    bool volumeAvailable() const { return mVolumeAvailable; }
    // A short note under the screenshot button that goes away by itself, as
    // when a screenshot was not saved; it does not stop anything.
    void showScreenshotNotice(const QString& text);
    QPushButton* backButton() const { return mBack; }
    QPushButton* homeButton() const { return mHome; }
    QPushButton* minimizeButton() const { return mMinimize; }
    QPushButton* maximizeButton() const { return mMaximize; }
    QPushButton* closeButton() const { return mClose; }

    // Where the maximize / restore button learns whether the window is
    // maximized; it asks whenever the window's state or size changes.
    // Without it the button takes the window to be maximized as Qt has it.
    void setMaximizedQuery(std::function<bool()> query);
    // Shows maximize or restore on the button, as the window is now.
    void updateMaximizeButton();
    // The mouse over the maximize / restore button when the window, not Qt,
    // sees it (WindowsLeanFrame): highlighted while |hot|, pressed while
    // |down|, with its tool tip after a while.
    void setMaximizeButtonMouse(bool hot, bool down);

signals:
    // setButtonsEnabled(false), as the safe shutdown begins.
    void buttonsDisabled();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QPushButton* addButton(const char* icon, const QString& tip, int iconSize);
    void updateTitle();
    void updateIcon();
    void showMaximizeToolTip();

    QWidget* mWindow;
    QLabel* mIcon = nullptr;
    QLabel* mTitle = nullptr;
    QPushButton* mScreenshot = nullptr;
    QPushButton* mVolume = nullptr;
    bool mVolumeAvailable = true;
    bool mButtonsEnabled = true;
    QPushButton* mBack = nullptr;
    QPushButton* mHome = nullptr;
    QFrame* mDivider = nullptr;
    QPushButton* mMinimize = nullptr;
    QPushButton* mMaximize = nullptr;
    QPushButton* mClose = nullptr;
    std::function<bool()> mMaximizedQuery;
    // Whether the button shows restore.
    bool mShowsRestore = false;
    bool mMaximizeHot = false;
    QTimer mMaximizeTipTimer;
};
