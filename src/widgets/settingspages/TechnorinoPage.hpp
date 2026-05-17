#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

class QLabel;
class QCheckBox;
class QComboBox;
class QPushButton;

namespace chatterino {

class GeneralPageView;
class DescriptionLabel;
struct DropdownArgs;

class TechnorinoPage : public SettingsPage
{
    Q_OBJECT

public:
    TechnorinoPage();

    bool filterElements(const QString &query) override;

private:
    void initLayout(GeneralPageView &layout);
    void initExtra();

    void updateTokenStatus(QLabel *label);
    void startDeviceCodeFlow(QPushButton *authBtn, QLabel *statusLabel,
                             QLabel *tokenStatusLabel);
    void pollDeviceCode(const QString &deviceCode, const QString &clientId,
                        int interval, int attempt, QPushButton *authBtn,
                        QLabel *statusLabel, QLabel *tokenStatusLabel);

    QString getFont(const DropdownArgs &args) const;

    DescriptionLabel *cachePath_{};
    GeneralPageView *view_{};
};

}  // namespace chatterino
