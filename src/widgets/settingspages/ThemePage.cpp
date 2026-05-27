// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/ThemePage.hpp"

#include "Application.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/helper/color/ColorButton.hpp"
#include "widgets/settingspages/GeneralPageView.hpp"
#include "widgets/settingspages/SettingWidget.hpp"
#include "widgets/helper/NotebookTab.hpp"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QFileDialog>

namespace chatterino {

namespace {

// Helper: creates a color button that shows the theme's actual color when the
// custom setting is empty, and live-updates when the theme changes.
void addThemeColorRow(GeneralPageView &view, const QString &label,
                      QStringSetting &setting,
                      std::function<QColor()> themeColorGetter,
                      std::vector<pajlada::Signals::Connection> &connections)
{
    auto *sw = SettingWidget::colorButton(label, setting, themeColorGetter);
    (void)sw->setTooltip("Current theme default: " + themeColorGetter().name(QColor::HexArgb));

    auto *themes = getApp()->getThemes();
    auto conn = themes->updated.connect([sw, &setting, themeColorGetter]() {
        // Update the button's color manually if the setting is empty
        // (SettingWidget::colorButton already handles updating when the setting itself changes)
        if (setting.getValue().isEmpty()) {
            // Because sw->actionWidget is private, we must find it
            auto *colorBtn = sw->findChild<ColorButton *>();
            if (colorBtn) {
                colorBtn->setColor(themeColorGetter());
            }
        }
        (void)sw->setTooltip("Current theme default: " + themeColorGetter().name(QColor::HexArgb));
    });
    connections.push_back(conn);

    sw->addTo(view);
}

}  // namespace

ThemePage::ThemePage()
{
    auto *y = new QVBoxLayout;
    auto *x = new QHBoxLayout;
    auto *view = GeneralPageView::withNavigation(this);
    
    x->addWidget(view);
    auto *z = new QFrame;
    z->setLayout(x);
    y->addWidget(z);
    this->setLayout(y);

    auto &s = *getSettings();
    auto *themes = getApp()->getThemes();

    view->addTitle("Theme Presets");

    {
        auto available = themes->availableThemes();
        available.emplace_back("System", "System");

        SettingWidget::dropdown("Theme", themes->themeName, available)
            ->addTo(*view);

        SettingWidget::dropdown("Dark system theme",
                                themes->darkSystemThemeName,
                                themes->availableThemes())
            ->setTooltip("This theme is selected if your system is in a dark "
                         "theme and you enabled the adaptive 'System' theme.")
            ->conditionallyEnabledBy(themes->themeName, "System")
            ->addTo(*view);

        SettingWidget::dropdown("Light system theme",
                                themes->lightSystemThemeName,
                                themes->availableThemes())
            ->setTooltip("This theme is selected if your system is in a light "
                         "theme and you enabled the adaptive 'System' theme.")
            ->conditionallyEnabledBy(themes->themeName, "System")
            ->addTo(*view);
    }

    ComboBox *tabDirectionDropdown =
        view->addDropdown<std::underlying_type_t<NotebookTabLocation>>(
            "Tab layout", {"Top", "Left", "Right", "Bottom"}, s.tabDirection,
            [](auto val) {
                switch (val)
                {
                    case NotebookTabLocation::Top:
                        return "Top";
                    case NotebookTabLocation::Left:
                        return "Left";
                    case NotebookTabLocation::Right:
                        return "Right";
                    case NotebookTabLocation::Bottom:
                        return "Bottom";
                }

                return "";
            },
            [](auto args) {
                if (args.value == "Bottom")
                {
                    return NotebookTabLocation::Bottom;
                }
                else if (args.value == "Left")
                {
                    return NotebookTabLocation::Left;
                }
                else if (args.value == "Right")
                {
                    return NotebookTabLocation::Right;
                }
                else
                {
                    // default to top
                    return NotebookTabLocation::Top;
                }
            },
            false);
    tabDirectionDropdown->setMinimumWidth(
        tabDirectionDropdown->minimumSizeHint().width());

    // ---- Custom Colors ----
    // Each color button previews the theme's actual color when no custom
    // override is set, and updates live when the theme changes.

    std::vector<pajlada::Signals::Connection> themeConnections;

    view->addTitle("Background Image");

    auto *bgWidget = SettingWidget::lineEdit("Background Image Path (leave empty to disable)", s.customBackgroundImage);
    (void)bgWidget->setTooltip("Absolute path to an image file (e.g. .png, .jpg, .gif) to use as the chat background.");

    auto *browseButton = new QPushButton("Browse");
    QObject::connect(browseButton, &QPushButton::clicked, [this]() {
        auto &s = *getSettings();
        QString fileName = QFileDialog::getOpenFileName(this, "Select Background Image", "", "Images (*.png *.jpg *.jpeg *.gif)");
        if (!fileName.isEmpty()) {
            s.customBackgroundImage = fileName;
        }
    });

    auto *hLayout = new QHBoxLayout;
    hLayout->addWidget(bgWidget);
    hLayout->addWidget(browseButton);
    view->addLayout(hLayout);

    SettingWidget::intInput("Background Image Opacity (%)", s.customBackgroundImageOpacity,
                            {0, 100, 1, "%"})
        ->addTo(*view);

    view->addTitle("Window & Tabs");

    addThemeColorRow(*view, "Accent Color (Tab Line & Indicators)", s.customAccentColor,
        [themes]() { return themes->accent; }, themeConnections);

    addThemeColorRow(*view, "Selected Tab Background", s.customSelectedTabBackground,
        [themes]() { return themes->tabs.selected.backgrounds.regular; }, themeConnections);

    addThemeColorRow(*view, "Window Background", s.customWindowBackground,
        [themes]() { return themes->window.background; }, themeConnections);

    addThemeColorRow(*view, "Live Button Indicator", s.customLiveButtonColor,
        [themes]() { return themes->tabs.liveIndicator; }, themeConnections);

    addThemeColorRow(*view, "Tab Divider Line", s.customTabDividerLine,
        [themes]() { return themes->tabs.dividerLine; }, themeConnections);

    addThemeColorRow(*view, "Tab Text", s.customTabTextColor,
        [themes]() { return themes->tabs.regular.text; }, themeConnections);

    addThemeColorRow(*view, "Selected Tab Text", s.customSelectedTabTextColor,
        [themes]() { return themes->tabs.selected.text; }, themeConnections);

    addThemeColorRow(*view, "Tab Hover Background", s.customTabHoverBackground,
        [themes]() { return themes->tabs.regular.backgrounds.hover; }, themeConnections);

    view->addTitle("Splits & Headers");

    addThemeColorRow(*view, "Split Background", s.customSplitBackground,
        [themes]() { return themes->splits.background; }, themeConnections);

    addThemeColorRow(*view, "Split Header Background", s.customSplitHeaderBackground,
        [themes]() { return themes->splits.header.background; }, themeConnections);

    addThemeColorRow(*view, "Split Header Border", s.customSplitHeaderBorder,
        [themes]() { return themes->splits.header.border; }, themeConnections);

    addThemeColorRow(*view, "Split Header Text", s.customSplitHeaderText,
        [themes]() { return themes->splits.header.text; }, themeConnections);

    addThemeColorRow(*view, "Message Separator", s.customMessageSeparator,
        [themes]() { return themes->splits.messageSeperator; }, themeConnections);

    addThemeColorRow(*view, "Chat Field Background", s.customChatFieldColor,
        [themes]() { return themes->splits.input.background; }, themeConnections);

    view->addTitle("Messages");

    addThemeColorRow(*view, "Message Background", s.customMessageBackground,
        [themes]() { return themes->messages.backgrounds.regular; }, themeConnections);

    addThemeColorRow(*view, "Alternate Message Background", s.customMessageAlternateBackground,
        [themes]() { return themes->messages.backgrounds.alternate; }, themeConnections);

    addThemeColorRow(*view, "Text Color", s.customTextColor,
        [themes]() { return themes->messages.textColors.regular; }, themeConnections);

    view->addTitle("Scrollbar");

    addThemeColorRow(*view, "Scrollbar Background", s.customScrollbarBackground,
        [themes]() { return themes->scrollbars.background; }, themeConnections);

    addThemeColorRow(*view, "Scrollbar Thumb", s.customScrollbarThumb,
        [themes]() { return themes->scrollbars.thumb; }, themeConnections);

    // Reset button
    auto *resetButton = new QPushButton("Reset All Custom Colors");
    QObject::connect(resetButton, &QPushButton::clicked, []() {
        auto &s = *getSettings();
        s.customAccentColor = "";
        s.customWindowBackground = "";
        s.customLiveButtonColor = "";
        s.customChatFieldColor = "";
        s.customSplitBackground = "";
        s.customSplitHeaderBackground = "";
        s.customSplitHeaderBorder = "";
        s.customSplitHeaderText = "";
        s.customMessageSeparator = "";
        s.customScrollbarThumb = "";
        s.customScrollbarBackground = "";
        s.customMessageBackground = "";
        s.customMessageAlternateBackground = "";
        s.customTextColor = "";
        s.customTabDividerLine = "";
        s.customTabTextColor = "";
        s.customSelectedTabTextColor = "";
        s.customTabHoverBackground = "";
        s.customSelectedTabBackground = "";
    });

    auto *resetLayout = new QHBoxLayout;
    resetLayout->addWidget(resetButton);
    resetLayout->addStretch(1);
    view->addLayout(resetLayout);

    view->addStretch();

    // Clean up theme connections when this page is destroyed
    QObject::connect(this, &QObject::destroyed, [themeConnections]() mutable {
        for (auto &conn : themeConnections) {
            conn.disconnect();
        }
    });
}

}  // namespace chatterino
