// SPDX-FileCopyrightText: 2024 Contributors to Banerino
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/api/Helix.hpp"
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

class PollBanner : public BaseWidget
{
public:
    explicit PollBanner(Split *parent);

    void setChannel(ChannelPtr channel);

protected:
    void paintEvent(QPaintEvent *event) override;
    void themeChangedEvent() override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void fetchPoll();
    void updateDisplay();
    void updateCountdown();
    void placeVote(const QString &choiceId);
    void startPolling();
    void stopPolling();
    static QString formatCount(int count);

    Split *split_;
    TwitchChannel *currentChannel_ = nullptr;
    pajlada::Signals::SignalHolder signalHolder_;

    std::optional<HelixPoll> poll_;
    QTimer pollTimer_;
    QTimer countdownTimer_;

    // UI elements
    QLabel *titleLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QLabel *timerLabel_ = nullptr;

    // Outcome widgets
    struct ChoiceWidget {
        QWidget *container = nullptr;
        QLabel *titleLabel = nullptr;
        QLabel *statsLabel = nullptr;
        QProgressBar *bar = nullptr;
        QPushButton *voteButton = nullptr;
    };
    std::vector<ChoiceWidget> choiceWidgets_;

    QVBoxLayout *choicesLayout_ = nullptr;
    bool minimized_ = false;
};

}  // namespace chatterino
