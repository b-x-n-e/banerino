// SPDX-FileCopyrightText: 2024 Contributors to Banerino
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/PollBanner.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/buttons/SvgButton.hpp"

#include <QHBoxLayout>
#include <QPainter>
#include <QStyle>

namespace chatterino {

PollBanner::PollBanner(Split *parent)
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
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(6);

    // Header row: title + status + timer
    auto *headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(6);

    this->titleLabel_ = new QLabel(this);
    this->titleLabel_->setWordWrap(true);
    this->titleLabel_->setCursor(Qt::PointingHandCursor);
    headerLayout->addWidget(this->titleLabel_, 1);

    this->statusLabel_ = new QLabel(this);
    headerLayout->addWidget(this->statusLabel_);

    this->timerLabel_ = new QLabel(this);
    headerLayout->addWidget(this->timerLabel_);

    this->endButton_ = new SvgButton(
        {
            .dark = ":/buttons/endPrediction-darkMode.svg",
            .light = ":/buttons/endPrediction-lightMode.svg",
        },
        this,
        QSize(2, 2));
    this->endButton_->setToolTip("End Poll early");
    this->endButton_->setFixedSize(34, 34);
    this->endButton_->setContentsMargins(0, 2, 0, 0);
    this->endButton_->setCursor(Qt::PointingHandCursor);
    this->endButton_->setHidden(true);
    headerLayout->addWidget(this->endButton_);

    this->cancelButton_ = new SvgButton(
        {
            .dark = ":/buttons/trashcan-darkMode.svg",
            .light = ":/buttons/trashcan-lightMode.svg",
        },
        this,
        QSize(2, 2));
    this->cancelButton_->setToolTip("Cancel Poll");
    this->cancelButton_->setFixedSize(26, 26);
    this->cancelButton_->setCursor(Qt::PointingHandCursor);
    this->cancelButton_->setHidden(true);
    headerLayout->addWidget(this->cancelButton_);

    layout->addLayout(headerLayout);

    // Choices area
    this->choicesLayout_ = new QVBoxLayout();
    this->choicesLayout_->setContentsMargins(0, 0, 0, 0);
    this->choicesLayout_->setSpacing(4);
    layout->addLayout(this->choicesLayout_);

    mainLayout->addWidget(container);
    this->setLayout(mainLayout);

    // Click header to toggle minimize
    this->titleLabel_->installEventFilter(this);
    this->statusLabel_->installEventFilter(this);
    this->timerLabel_->installEventFilter(this);

    // Countdown timer - ticks every second
    QObject::connect(&this->countdownTimer_, &QTimer::timeout, [this]() {
        this->updateCountdown();
    });

    // Poll timer - fetches poll data periodically
    QObject::connect(&this->pollTimer_, &QTimer::timeout, [this]() {
        this->fetchPoll();
    });

    QObject::connect(this->endButton_, &Button::leftClicked, [this]() {
        if (this->currentChannel_ && this->poll_.has_value()) {
            auto pollId = this->poll_->id;
            this->currentChannel_->terminatePoll(
                pollId,
                [this]() { this->fetchPoll(); },
                [this](const QString &error) {
                    this->currentChannel_->addSystemMessage(
                        QString("Failed to end poll: %1").arg(error));
                });
        }
    });

    QObject::connect(this->cancelButton_, &Button::leftClicked, [this]() {
        if (this->currentChannel_ && this->poll_.has_value()) {
            auto pollId = this->poll_->id;
            this->currentChannel_->archivePoll(
                pollId,
                [this]() { this->fetchPoll(); },
                [this](const QString &error) {
                    this->currentChannel_->addSystemMessage(
                        QString("Failed to cancel poll: %1").arg(error));
                });
        }
    });

    this->themeChangedEvent();
}

void PollBanner::setChannel(ChannelPtr channel)
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

    this->currentChannel_ = tc;

    // Fetch immediately, then start polling
    this->fetchPoll();
    this->startPolling();
}

void PollBanner::fetchPoll()
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

    this->currentChannel_->getActivePoll(
        [this](std::optional<HelixPoll> result) {
            if (!result.has_value())
            {
                // No active poll - hide
                if (this->poll_.has_value())
                {
                    this->poll_.reset();
                    QMetaObject::invokeMethod(this, [this]() {
                        this->hide();
                        this->setFixedHeight(0);
                    });
                }
                return;
            }

            const auto &hp = result.value();

            // Statuses: ACTIVE, COMPLETED, TERMINATED, ARCHIVED, MODERATED, INVALID
            // Only show ACTIVE or recently ended ones
            if (hp.status != "ACTIVE")
            {
                if (this->poll_.has_value() && this->poll_->status == "ACTIVE")
                {
                    // Poll just ended - show briefly then hide
                    this->poll_ = hp;
                    QMetaObject::invokeMethod(this, [this]() {
                        this->updateDisplay();
                        // Auto-hide after 8 seconds for ended polls
                        QTimer::singleShot(8000, this, [this]() {
                            this->poll_.reset();
                            this->hide();
                            this->setFixedHeight(0);
                        });
                    });
                }
                else if (!this->poll_.has_value())
                {
                    // Not currently showing, don't show old resolved ones
                    return;
                }
                return;
            }

            this->poll_ = hp;
            QMetaObject::invokeMethod(this, [this]() {
                this->updateDisplay();
            });
        },
        [](const QString & /*error*/) {
            // Silently fail - will retry on next poll
        });
}

void PollBanner::updateDisplay()
{
    if (!this->poll_.has_value())
    {
        this->endButton_->hide();
        this->cancelButton_->hide();
        this->hide();
        this->setFixedHeight(0);
        return;
    }

    const auto &poll = this->poll_.value();

    bool isModOrBroadcaster = this->currentChannel_ && (this->currentChannel_->isMod() || this->currentChannel_->isBroadcaster());
    bool isActive = (poll.status == "ACTIVE");
    this->endButton_->setVisible(isModOrBroadcaster && isActive);
    this->cancelButton_->setVisible(isModOrBroadcaster && isActive);

    this->titleLabel_->setText(poll.title);

    auto font = getApp()->getFonts()->getFont(FontStyle::ChatMedium, this->scale());
    auto fontBold = getApp()->getFonts()->getFont(FontStyle::ChatMediumBold, this->scale());

    this->titleLabel_->setFont(fontBold);
    this->statusLabel_->setFont(fontBold);
    this->timerLabel_->setFont(fontBold);

    // Status badge
    QString statusText;
    QString statusStyle;
    if (poll.status == "ACTIVE")
    {
        statusText = "POLL";
        statusStyle =
            QString("background: %1; color: white; padding: 2px 6px; "
            "border-radius: 3px; font-weight: bold;").arg(this->theme->accent.name());
    }
    else if (poll.status == "COMPLETED")
    {
        statusText = "COMPLETED";
        statusStyle =
            "background: #00ad03; color: white; padding: 2px 6px; "
            "border-radius: 3px; font-size: 10px; font-weight: bold;";
    }
    else if (poll.status == "TERMINATED")
    {
        statusText = "ENDED EARLY";
        statusStyle =
            "background: #868686; color: white; padding: 2px 6px; "
            "border-radius: 3px; font-size: 10px; font-weight: bold;";
    }
    else
    {
        statusText = poll.status;
        statusStyle =
            "background: #868686; color: white; padding: 2px 6px; "
            "border-radius: 3px; font-size: 10px; font-weight: bold;";
    }
    this->statusLabel_->setText(statusText);
    this->statusLabel_->setStyleSheet(statusStyle);

    // Calculate total votes across all choices
    int totalVotes = 0;
    int maxVotes = -1;
    for (const auto &c : poll.choices)
    {
        totalVotes += c.votes;
        if (c.votes > maxVotes)
        {
            maxVotes = c.votes;
        }
    }

    // Clear old outcome widgets
    for (auto &cw : this->choiceWidgets_)
    {
        if (cw.container)
        {
            this->choicesLayout_->removeWidget(cw.container);
            cw.container->deleteLater();
        }
    }
    this->choiceWidgets_.clear();

    // Build choice widgets
    for (size_t i = 0; i < poll.choices.size(); i++)
    {
        const auto &choice = poll.choices[i];
        ChoiceWidget cw;

        int pct = totalVotes > 0 ? (choice.votes * 100 / totalVotes) : 0;
        bool isWinner = (poll.status == "COMPLETED" && choice.votes == maxVotes && totalVotes > 0);
        bool isLeading = (poll.status == "ACTIVE" && choice.votes == maxVotes && totalVotes > 0);

        // Container with the progress bar as background
        cw.container = new QWidget(this);
        cw.container->setFixedHeight(32);

        // Use a stacked layout approach: draw bg via stylesheet gradient
        QColor barColor = isWinner ? QColor("#00ad03") 
                        : isLeading ? this->theme->accent 
                        : this->theme->accent.darker(130);

        // Use a linear gradient to simulate the progress bar fill
        QString bgStyle = QString(
            "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
            "stop:0 %1, stop:%2 %1, stop:%3 %4, stop:1 %4); "
            "border-radius: 5px;")
            .arg(barColor.name())
            .arg(pct / 100.0, 0, 'f', 4)
            .arg(pct / 100.0 + 0.001, 0, 'f', 4)
            .arg("rgba(255,255,255,0.08)");
        cw.container->setStyleSheet(bgStyle);

        auto *rowLayout = new QHBoxLayout(cw.container);
        rowLayout->setContentsMargins(10, 0, 8, 0);
        rowLayout->setSpacing(6);

        cw.titleLabel = new QLabel(choice.title, cw.container);
        cw.titleLabel->setFont(fontBold);
        cw.titleLabel->setStyleSheet("background: transparent; color: white;");
        rowLayout->addWidget(cw.titleLabel, 1);

        // Vote count
        QString statsStr = choice.votes > 0
            ? QString("%1").arg(formatCount(choice.votes))
            : "0";
        cw.statsLabel = new QLabel(statsStr, cw.container);
        cw.statsLabel->setFont(font);
        cw.statsLabel->setStyleSheet("background: transparent; color: rgba(255,255,255,0.7);");
        rowLayout->addWidget(cw.statsLabel);

        // Percentage
        auto *pctLabel = new QLabel(QString("%1%").arg(pct), cw.container);
        pctLabel->setFont(fontBold);
        pctLabel->setStyleSheet("background: transparent; color: white; min-width: 30px;");
        pctLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rowLayout->addWidget(pctLabel);

        if (poll.status == "ACTIVE")
        {
            cw.voteButton = new QPushButton("Vote", cw.container);
            cw.voteButton->setFont(fontBold);
            cw.voteButton->setCursor(Qt::PointingHandCursor);
            cw.voteButton->setFixedHeight(24);
            cw.voteButton->setMinimumWidth(44);

            cw.voteButton->setStyleSheet(
                "QPushButton { background: rgba(255,255,255,0.2); color: white; border: none; "
                "border-radius: 4px; font-weight: bold; "
                "padding: 0px 8px; }"
                "QPushButton:hover { background: rgba(255,255,255,0.4); }");

            QString choiceId = choice.id;
            QObject::connect(
                cw.voteButton, &QPushButton::clicked, [this, choiceId]() {
                    this->placeVote(choiceId);
                });

            rowLayout->addWidget(cw.voteButton);
        }

        // Hide the QProgressBar - we're drawing the fill via CSS gradient now
        cw.bar = nullptr;

        this->choiceWidgets_.push_back(cw);
        this->choicesLayout_->addWidget(cw.container);
    }

    // Start countdown if active
    if (poll.status == "ACTIVE" && !this->countdownTimer_.isActive())
    {
        this->countdownTimer_.start(1000);
    }
    else if (poll.status != "ACTIVE")
    {
        this->countdownTimer_.stop();
        this->timerLabel_->setText("");
    }

    // Hide/show choices based on minimize state
    for (auto &cw : this->choiceWidgets_)
    {
        if (cw.container)
        {
            cw.container->setVisible(!this->minimized_);
        }
    }

    if (this->minimized_)
    {
        this->setFixedHeight(36);
    }
    else
    {
        // Calculate height based on content
        // Header ~30, each choice ~36
        int choiceHeight = 36;
        int height = 40 + (static_cast<int>(poll.choices.size()) * choiceHeight);
        this->setFixedHeight(height);
    }
    this->show();
}

void PollBanner::updateCountdown()
{
    if (!this->poll_.has_value())
    {
        this->countdownTimer_.stop();
        return;
    }

    const auto &poll = this->poll_.value();
    if (poll.status != "ACTIVE" || poll.duration <= 0 || !poll.startedAt.isValid())
    {
        this->countdownTimer_.stop();
        this->timerLabel_->setText("");
        return;
    }

    auto elapsed = poll.startedAt.secsTo(QDateTime::currentDateTimeUtc());
    auto remaining = poll.duration - elapsed;

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

    // Trigger repaint to update the shrinking line
    this->update();
}

void PollBanner::placeVote(const QString &choiceId)
{
    if (!this->currentChannel_ || !this->poll_.has_value())
    {
        return;
    }

    auto pollId = this->poll_->id;
    this->currentChannel_->voteInPoll(
        pollId, choiceId,
        [this]() {
            // Vote successful, maybe wait for next fetch or fetch immediately
            this->fetchPoll();
        },
        [this](const QString &error) {
            this->currentChannel_->addSystemMessage(
                QString("Failed to vote in poll: %1").arg(error));
        });
}

void PollBanner::startPolling()
{
    // Poll every 5 seconds for poll updates
    this->pollTimer_.start(5000);
}

void PollBanner::stopPolling()
{
    this->pollTimer_.stop();
    this->countdownTimer_.stop();
}

QString PollBanner::formatCount(int count)
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

void PollBanner::paintEvent(QPaintEvent *event)
{
    BaseWidget::paintEvent(event);

    QPainter painter(this);

    // Draw border
    auto border = this->theme->splits.header.border;
    painter.setPen(border);
    painter.drawRect(0, 0, this->width() - 1, this->height() - 1);

    // Accent left bar
    painter.fillRect(0, 0, 4, this->height(), this->theme->accent);

    // Shrinking time line at the bottom
    if (this->poll_.has_value() && this->poll_->status == "ACTIVE" && this->poll_->duration > 0 && this->poll_->startedAt.isValid())
    {
        auto elapsed = this->poll_->startedAt.secsTo(QDateTime::currentDateTimeUtc());
        auto remaining = this->poll_->duration - elapsed;
        if (remaining > 0)
        {
            double ratio = static_cast<double>(remaining) / this->poll_->duration;
            int lineWidth = static_cast<int>(this->width() * ratio);
            painter.fillRect(0, this->height() - 2, lineWidth, 2, Qt::white);
        }
    }
}

void PollBanner::themeChangedEvent()
{
    auto bg = this->theme->splits.background;
    this->setStyleSheet(
        QString("PollBanner { background-color: %1; }")
            .arg(bg.darker(110).name()));

    if (this->titleLabel_)
    {
        this->titleLabel_->setStyleSheet(
            QString("font-weight: bold; font-size: 13px; color: %1;")
                .arg(this->theme->messages.textColors.regular.name()));
    }

    if (this->endButton_)
    {
        auto scale = this->scale();
        this->endButton_->setFixedHeight(int(34 * scale));
        this->endButton_->setFixedWidth(int(34 * scale));
    }

    if (this->cancelButton_)
    {
        auto scale = this->scale();
        this->cancelButton_->setFixedHeight(int(26 * scale));
        this->cancelButton_->setFixedWidth(int(26 * scale));
    }

    if (this->poll_.has_value())
    {
        this->updateDisplay();
    }
}

bool PollBanner::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease &&
        (obj == this->titleLabel_ || obj == this->statusLabel_ || obj == this->timerLabel_))
    {
        this->minimized_ = !this->minimized_;
        if (this->poll_.has_value())
        {
            this->updateDisplay();
        }
        return true;
    }
    return BaseWidget::eventFilter(obj, event);
}

}  // namespace chatterino

