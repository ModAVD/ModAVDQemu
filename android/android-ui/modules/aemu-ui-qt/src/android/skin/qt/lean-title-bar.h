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
#include <QWidget>

class QEvent;
class QFrame;
class QLabel;
class QPushButton;
class QResizeEvent;

// The title bar the lean window draws itself (ModAVD#58), at the top of the
// emulator window: the window's icon and title on the left, then back, home,
// a divider, minimize and close on the right. It shows the title and icon of
// the window it sits in. Moving the window by the title bar, and the system
// menu, are the window's (WindowsLeanFrame); this only has the buttons.
class LeanTitleBar : public QWidget {
    Q_OBJECT

public:
    // Logical pixels.
    static constexpr int kHeight = 32;

    explicit LeanTitleBar(QWidget* window);

    // Where the buttons start, in logical pixels from the left edge.
    int buttonsLeft() const;
    void setButtonsEnabled(bool enabled);

    QPushButton* backButton() const { return mBack; }
    QPushButton* homeButton() const { return mHome; }
    QPushButton* minimizeButton() const { return mMinimize; }
    QPushButton* closeButton() const { return mClose; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QPushButton* addButton(const char* icon, const QString& tip, int iconSize);
    void updateTitle();
    void updateIcon();

    QWidget* mWindow;
    QLabel* mIcon = nullptr;
    QLabel* mTitle = nullptr;
    QPushButton* mBack = nullptr;
    QPushButton* mHome = nullptr;
    QFrame* mDivider = nullptr;
    QPushButton* mMinimize = nullptr;
    QPushButton* mClose = nullptr;
};
