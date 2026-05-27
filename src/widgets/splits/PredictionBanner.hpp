// SPDX-FileCopyrightText: 2024 Contributors to Banerino
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>

#include <QDateTime>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>
#include <optional>
#include <vector>

namespace chatterino {

class Channel;
using ChannelPtr = std::shared_ptr<Channel>;
class TwitchChannel;
class Split;
class SvgButton;

struct PredictionOutcome {
    QString id;
    QString title;
    QString color;  // "BLUE" or "PINK"
    int totalPoints = 0;
    int totalUsers = 0;
};

struct PredictionData {
    QString id;
    QString title;
    QString status;  // ACTIVE, LOCKED, RESOLVE_PENDING, RESOLVED, CANCELED
    QString winningOutcomeId;
    QDateTime createdAt;
    QDateTime lockedAt;
    QDateTime endedAt;
    int predictionWindowSeconds = 0;
    std::vector<PredictionOutcome> outcomes;
};

class PredictionBanner : public BaseWidget
{
public:
    explicit PredictionBanner(Split *parent);

    void setChannel(ChannelPtr channel);

protected:
    void paintEvent(QPaintEvent *event) override;
    void themeChangedEvent() override;

private:
    void fetchPrediction();
    void updateDisplay();
    void updateCountdown();
    void placeBet(const QString &outcomeId, int points);
    void startPolling();
    void stopPolling();
    static QString formatCount(int count);

    Split *split_;
    TwitchChannel *currentChannel_ = nullptr;
    pajlada::Signals::SignalHolder signalHolder_;

    std::optional<PredictionData> prediction_;
    QTimer pollTimer_;
    QTimer countdownTimer_;

    // UI elements
    QLabel *titleLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QLabel *timerLabel_ = nullptr;
    QLabel *pointsLabel_ = nullptr;
    SvgButton *endButton_ = nullptr;
    SvgButton *cancelButton_ = nullptr;

    // Outcome widgets
    struct OutcomeWidget {
        QWidget *container = nullptr;
        QLabel *titleLabel = nullptr;
        QLabel *statsLabel = nullptr;
        QProgressBar *bar = nullptr;
        QPushButton *betButton = nullptr;
    };
    std::vector<OutcomeWidget> outcomeWidgets_;

    QVBoxLayout *outcomesLayout_ = nullptr;
    QWidget *betInputContainer_ = nullptr;
    int pendingBetAmount_ = 10;
};

}  // namespace chatterino
