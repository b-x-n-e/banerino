// SPDX-FileCopyrightText: 2024 Contributors to Banerino
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWindow.hpp"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace chatterino {

class TwitchChannel;

class ManagePredictionDialog : public BaseWindow
{
    Q_OBJECT

public:
    explicit ManagePredictionDialog(TwitchChannel *channel, QWidget *parent = nullptr);

protected:
    void themeChangedEvent() override;

private:
    void buildUi();
    void fetchPrediction();
    void lockPrediction();
    void cancelPrediction();
    void completePrediction(const QString &outcomeId);

    TwitchChannel *channel_;
    QVBoxLayout *mainLayout_;
    QLabel *statusLabel_;
    QWidget *contentWidget_;
    
    QString currentPredictionId_;
};

}  // namespace chatterino
