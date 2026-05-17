// SPDX-FileCopyrightText: 2024 Contributors to Banerino
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/PredictionBanner.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "singletons/Theme.hpp"
#include "widgets/splits/Split.hpp"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QPainter>
#include <QStyle>

namespace chatterino {

PredictionBanner::PredictionBanner(Split *parent)
    : BaseWidget(parent)
    , split_(parent)
{
    this->setFixedHeight(0);
    this->hide();

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);

    // Header row: title + status + timer
    auto *headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(6);

    this->titleLabel_ = new QLabel(this);
    this->titleLabel_->setWordWrap(true);
    headerLayout->addWidget(this->titleLabel_, 1);

    this->pointsLabel_ = new QLabel(this);
    this->pointsLabel_->setStyleSheet("color: #00e6cb; font-weight: bold; font-size: 11px;");
    headerLayout->addWidget(this->pointsLabel_);

    this->statusLabel_ = new QLabel(this);
    headerLayout->addWidget(this->statusLabel_);

    this->timerLabel_ = new QLabel(this);
    headerLayout->addWidget(this->timerLabel_);

    layout->addLayout(headerLayout);

    // Outcomes area
    this->outcomesLayout_ = new QVBoxLayout();
    this->outcomesLayout_->setContentsMargins(0, 0, 0, 0);
    this->outcomesLayout_->setSpacing(4);
    layout->addLayout(this->outcomesLayout_);

    // Bet input (shown only during ACTIVE state)
    this->betInputContainer_ = new QWidget(this);
    auto *betLayout = new QHBoxLayout(this->betInputContainer_);
    betLayout->setContentsMargins(0, 4, 0, 0);
    betLayout->setSpacing(4);

    auto *betInput = new QLineEdit(this->betInputContainer_);
    betInput->setPlaceholderText("Points...");
    betInput->setMaximumWidth(90);
    betInput->setObjectName("predictionBetInput");
    betLayout->addWidget(betInput);

    // Quick bet buttons
    for (const auto &[label, amount] :
         std::vector<std::pair<QString, int>>{
             {"10", 10}, {"100", 100}, {"1K", 1000}, {"5K", 5000}})
    {
        auto *btn = new QPushButton(label, this->betInputContainer_);
        btn->setFixedHeight(24);
        btn->setMinimumWidth(36);
        btn->setObjectName("predictionQuickBet");
        betLayout->addWidget(btn);

        QObject::connect(btn, &QPushButton::clicked, [this, amount]() {
            if (!this->prediction_.has_value() ||
                this->prediction_->status != "ACTIVE")
            {
                return;
            }
            // Store amount for use with outcome selection
            this->pendingBetAmount_ = amount;
        });
    }

    betLayout->addStretch(1);
    layout->addWidget(this->betInputContainer_);
    this->betInputContainer_->hide();

    mainLayout->addWidget(container);
    this->setLayout(mainLayout);

    // Countdown timer - ticks every second
    QObject::connect(&this->countdownTimer_, &QTimer::timeout, [this]() {
        this->updateCountdown();
    });

    // Poll timer - fetches prediction data periodically
    QObject::connect(&this->pollTimer_, &QTimer::timeout, [this]() {
        this->fetchPrediction();
    });

    this->themeChangedEvent();
}

void PredictionBanner::setChannel(ChannelPtr channel)
{
    this->signalHolder_.clear();
    this->currentChannel_ = nullptr;
    this->stopPolling();

    auto *tc = dynamic_cast<TwitchChannel *>(channel.get());
    if (!tc)
    {
        this->hide();
        this->setFixedHeight(0);
        return;
    }

    this->currentChannel_ = tc;

    this->signalHolder_.managedConnect(
        this->currentChannel_->channelPointsBalanceUpdated,
        [this](int balance) {
            this->pointsLabel_->setText(
                QString("🪙 %1").arg(formatCount(balance)));
        });

    // Fetch immediately, then start polling
    this->fetchPrediction();
    this->currentChannel_->refreshChannelPointsBalance();
    this->startPolling();
}

void PredictionBanner::fetchPrediction()
{
    if (!this->currentChannel_)
    {
        return;
    }

    auto roomId = this->currentChannel_->roomId();
    if (roomId.isEmpty())
    {
        return;
    }

    getHelix()->getPredictions(
        roomId, {}, 1, "",
        [this](const HelixPredictions &result) {
            if (result.predictions.empty())
            {
                // No predictions - hide
                if (this->prediction_.has_value())
                {
                    this->prediction_.reset();
                    QMetaObject::invokeMethod(this, [this]() {
                        this->hide();
                        this->setFixedHeight(0);
                    });
                }
                return;
            }

            const auto &hp = result.predictions.front();

            // Only show ACTIVE or LOCKED predictions
            if (hp.status != "ACTIVE" && hp.status != "LOCKED")
            {
                if (this->prediction_.has_value() &&
                    (this->prediction_->status == "ACTIVE" ||
                     this->prediction_->status == "LOCKED"))
                {
                    // Prediction just resolved/canceled - show briefly then hide
                    PredictionData data;
                    data.id = hp.id;
                    data.title = hp.title;
                    data.status = hp.status;
                    data.winningOutcomeId = hp.winningOutcomeID;

                    for (const auto &o : hp.outcomes)
                    {
                        PredictionOutcome outcome;
                        outcome.id = o.id;
                        outcome.title = o.title;
                        outcome.totalPoints = o.channelPoints;
                        outcome.totalUsers = o.users;
                        outcome.color =
                            (&o == &hp.outcomes.front()) ? "BLUE" : "PINK";
                        data.outcomes.push_back(outcome);
                    }

                    this->prediction_ = data;
                    QMetaObject::invokeMethod(this, [this]() {
                        this->updateDisplay();
                        // Auto-hide after 8 seconds for resolved/canceled
                        QTimer::singleShot(8000, this, [this]() {
                            this->prediction_.reset();
                            this->hide();
                            this->setFixedHeight(0);
                        });
                    });
                }
                else if (!this->prediction_.has_value())
                {
                    // Not currently showing, don't show old resolved ones
                    return;
                }
                return;
            }

            PredictionData data;
            data.id = hp.id;
            data.title = hp.title;
            data.status = hp.status;
            data.winningOutcomeId = hp.winningOutcomeID;

            for (const auto &o : hp.outcomes)
            {
                PredictionOutcome outcome;
                outcome.id = o.id;
                outcome.title = o.title;
                outcome.totalPoints = o.channelPoints;
                outcome.totalUsers = o.users;
                // First outcome is BLUE, second is PINK (Twitch convention)
                outcome.color =
                    (&o == &hp.outcomes.front()) ? "BLUE" : "PINK";
                data.outcomes.push_back(outcome);
            }

            this->prediction_ = data;
            QMetaObject::invokeMethod(this, [this]() {
                this->updateDisplay();
            });
        },
        [](const QString & /*error*/) {
            // Silently fail - will retry on next poll
        });
}

void PredictionBanner::updateDisplay()
{
    if (!this->prediction_.has_value())
    {
        this->hide();
        this->setFixedHeight(0);
        return;
    }

    const auto &pred = this->prediction_.value();

    // Title
    this->titleLabel_->setText(pred.title);

    // Status badge
    QString statusText;
    QString statusStyle;
    if (pred.status == "ACTIVE")
    {
        statusText = "VOTING";
        statusStyle =
            "background: #9147ff; color: white; padding: 2px 6px; "
            "border-radius: 3px; font-size: 10px; font-weight: bold;";
    }
    else if (pred.status == "LOCKED")
    {
        statusText = "LOCKED";
        statusStyle =
            "background: #e91916; color: white; padding: 2px 6px; "
            "border-radius: 3px; font-size: 10px; font-weight: bold;";
    }
    else if (pred.status == "RESOLVED")
    {
        statusText = "RESOLVED";
        statusStyle =
            "background: #00ad03; color: white; padding: 2px 6px; "
            "border-radius: 3px; font-size: 10px; font-weight: bold;";
    }
    else if (pred.status == "CANCELED")
    {
        statusText = "REFUNDED";
        statusStyle =
            "background: #868686; color: white; padding: 2px 6px; "
            "border-radius: 3px; font-size: 10px; font-weight: bold;";
    }
    this->statusLabel_->setText(statusText);
    this->statusLabel_->setStyleSheet(statusStyle);

    // Show/hide bet input
    this->betInputContainer_->setVisible(pred.status == "ACTIVE");

    // Calculate total points across all outcomes
    int totalPoints = 0;
    for (const auto &o : pred.outcomes)
    {
        totalPoints += o.totalPoints;
    }

    // Clear old outcome widgets
    for (auto &ow : this->outcomeWidgets_)
    {
        if (ow.container)
        {
            this->outcomesLayout_->removeWidget(ow.container);
            ow.container->deleteLater();
        }
    }
    this->outcomeWidgets_.clear();

    // Build outcome widgets
    for (size_t i = 0; i < pred.outcomes.size(); i++)
    {
        const auto &outcome = pred.outcomes[i];
        OutcomeWidget ow;

        ow.container = new QWidget(this);
        auto *rowLayout = new QVBoxLayout(ow.container);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(2);

        // Top row: title + stats
        auto *topRow = new QHBoxLayout();
        topRow->setContentsMargins(0, 0, 0, 0);

        ow.titleLabel = new QLabel(outcome.title, ow.container);
        ow.titleLabel->setStyleSheet(
            "font-weight: bold; font-size: 12px;");
        topRow->addWidget(ow.titleLabel, 1);

        // Stats: users & points
        QString statsStr;
        if (outcome.totalUsers > 0)
        {
            statsStr = QString("%1 users · %2 pts")
                           .arg(formatCount(outcome.totalUsers))
                           .arg(formatCount(outcome.totalPoints));
        }
        else
        {
            statsStr = "No votes";
        }

        ow.statsLabel = new QLabel(statsStr, ow.container);
        ow.statsLabel->setStyleSheet("font-size: 11px; color: #aaa;");
        topRow->addWidget(ow.statsLabel);

        // Percentage
        int pct = totalPoints > 0
                      ? (outcome.totalPoints * 100 / totalPoints)
                      : 0;

        auto *pctLabel = new QLabel(QString("%1%").arg(pct), ow.container);
        pctLabel->setStyleSheet(
            "font-weight: bold; font-size: 12px; min-width: 35px;");
        pctLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        topRow->addWidget(pctLabel);

        rowLayout->addLayout(topRow);

        // Progress bar
        ow.bar = new QProgressBar(ow.container);
        ow.bar->setMinimum(0);
        ow.bar->setMaximum(100);
        ow.bar->setValue(pct);
        ow.bar->setTextVisible(false);
        ow.bar->setFixedHeight(8);

        // Color: BLUE (#387aff) or PINK (#f5009b)
        QColor barColor = (outcome.color == "BLUE")
                              ? QColor("#387aff")
                              : QColor("#f5009b");

        // Winning outcome gets green
        bool isWinner = (!pred.winningOutcomeId.isEmpty() &&
                         outcome.id == pred.winningOutcomeId);
        if (isWinner)
        {
            barColor = QColor("#00ad03");
        }

        ow.bar->setStyleSheet(
            QString(
                "QProgressBar { background: rgba(255,255,255,0.1); "
                "border: none; border-radius: 4px; }"
                "QProgressBar::chunk { background: %1; border-radius: 4px; }")
                .arg(barColor.name()));

        rowLayout->addWidget(ow.bar);

        // Vote button (only during ACTIVE)
        if (pred.status == "ACTIVE")
        {
            ow.betButton = new QPushButton(
                QString("Vote %1").arg(outcome.title), ow.container);
            ow.betButton->setFixedHeight(26);
            ow.betButton->setCursor(Qt::PointingHandCursor);

            QString btnColor = (outcome.color == "BLUE")
                                   ? "#387aff"
                                   : "#f5009b";
            ow.betButton->setStyleSheet(
                QString(
                    "QPushButton { background: %1; color: white; border: none; "
                    "border-radius: 4px; font-weight: bold; font-size: 11px; "
                    "padding: 2px 12px; }"
                    "QPushButton:hover { background: %2; }")
                    .arg(btnColor)
                    .arg(QColor(btnColor).lighter(120).name()));

            QString outcomeId = outcome.id;
            QObject::connect(
                ow.betButton, &QPushButton::clicked, [this, outcomeId]() {
                    int amount = this->pendingBetAmount_;
                    if (amount <= 0)
                    {
                        amount = 10;  // Default bet
                    }
                    this->placeBet(outcomeId, amount);
                });

            rowLayout->addWidget(ow.betButton);
        }

        this->outcomeWidgets_.push_back(ow);
        this->outcomesLayout_->addWidget(ow.container);
    }

    // Start countdown if active
    if (pred.status == "ACTIVE" && !this->countdownTimer_.isActive())
    {
        this->countdownTimer_.start(1000);
    }
    else if (pred.status != "ACTIVE")
    {
        this->countdownTimer_.stop();
        this->timerLabel_->setText("");
    }

    // Calculate height based on content
    // Header ~30, each outcome ~60 (with button) or ~40 (without), bet area ~30
    int outcomeHeight = pred.status == "ACTIVE" ? 70 : 44;
    int height = 40 + (static_cast<int>(pred.outcomes.size()) * outcomeHeight);
    if (pred.status == "ACTIVE")
    {
        height += 36;  // bet input area
    }

    this->setFixedHeight(height);
    this->show();
}

void PredictionBanner::updateCountdown()
{
    if (!this->prediction_.has_value())
    {
        this->countdownTimer_.stop();
        return;
    }

    const auto &pred = this->prediction_.value();
    if (pred.status != "ACTIVE" || pred.predictionWindowSeconds <= 0)
    {
        this->countdownTimer_.stop();
        this->timerLabel_->setText("");
        return;
    }

    auto elapsed = pred.createdAt.secsTo(QDateTime::currentDateTimeUtc());
    auto remaining = pred.predictionWindowSeconds - elapsed;

    if (remaining <= 0)
    {
        this->timerLabel_->setText("0:00");
        this->countdownTimer_.stop();
        return;
    }

    int mins = remaining / 60;
    int secs = remaining % 60;
    this->timerLabel_->setText(
        QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0')));

    // Pulse effect when running low
    if (remaining <= 10)
    {
        this->timerLabel_->setStyleSheet(
            "font-weight: bold; font-size: 14px; color: #ff4444;");
    }
    else
    {
        this->timerLabel_->setStyleSheet(
            "font-weight: bold; font-size: 14px; color: #ccc;");
    }
}

void PredictionBanner::placeBet(const QString &outcomeId, int points)
{
    if (!this->currentChannel_ || !this->prediction_.has_value())
    {
        return;
    }

    // Placing bets requires GQL (not supported via Helix for viewers).
    // For now, show a system message indicating the bet would be placed.
    // Full GQL betting can be added later with the auth token from pinning.
    this->currentChannel_->addSystemMessage(
        QString("Prediction voting from Banerino is display-only for now. "
                "Vote on Twitch to place %1 points on this outcome.")
            .arg(points));
}

void PredictionBanner::startPolling()
{
    // Poll every 5 seconds for prediction updates
    this->pollTimer_.start(5000);
}

void PredictionBanner::stopPolling()
{
    this->pollTimer_.stop();
    this->countdownTimer_.stop();
}

QString PredictionBanner::formatCount(int count)
{
    if (count >= 1000000)
    {
        return QString("%1M").arg(count / 1000000.0, 0, 'f', 1);
    }
    if (count >= 1000)
    {
        return QString("%1K").arg(count / 1000.0, 0, 'f', 1);
    }
    return QString::number(count);
}

void PredictionBanner::paintEvent(QPaintEvent *event)
{
    BaseWidget::paintEvent(event);

    QPainter painter(this);

    // Draw border
    auto border = this->theme->splits.header.border;
    painter.setPen(border);
    painter.drawRect(0, 0, this->width() - 1, this->height() - 1);

    // Purple accent left bar (Twitch prediction purple)
    painter.fillRect(0, 0, 4, this->height(), QColor("#9147ff"));
}

void PredictionBanner::themeChangedEvent()
{
    auto bg = this->theme->splits.background;
    this->setStyleSheet(
        QString("PredictionBanner { background-color: %1; }")
            .arg(bg.darker(110).name()));

    if (this->titleLabel_)
    {
        this->titleLabel_->setStyleSheet(
            QString("font-weight: bold; font-size: 13px; color: %1;")
                .arg(this->theme->messages.textColors.regular.name()));
    }

    if (this->prediction_.has_value())
    {
        this->updateDisplay();
    }
}

}  // namespace chatterino
