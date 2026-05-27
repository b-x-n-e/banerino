// SPDX-FileCopyrightText: 2024 Contributors to Banerino
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/ManagePredictionDialog.hpp"

#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "singletons/Theme.hpp"
#include "util/FormatTime.hpp"

#include <QMessageBox>

namespace chatterino {

ManagePredictionDialog::ManagePredictionDialog(TwitchChannel *channel, QWidget *parent)
    : BaseWindow({BaseWindow::Flags::Dialog}, parent)
    , channel_(channel)
{
    this->setWindowTitle("Manage Active Prediction");
    this->resize(450, 400);

    this->mainLayout_ = new QVBoxLayout(this->getLayoutContainer());
    this->mainLayout_->setContentsMargins(20, 20, 20, 20);
    this->mainLayout_->setSpacing(16);

    this->statusLabel_ = new QLabel("Loading prediction...", this);
    this->statusLabel_->setStyleSheet("font-size: 16px; font-weight: bold;");
    this->statusLabel_->setAlignment(Qt::AlignCenter);
    this->mainLayout_->addWidget(this->statusLabel_);

    this->contentWidget_ = new QWidget(this);
    this->mainLayout_->addWidget(this->contentWidget_);

    this->mainLayout_->addStretch(1);

    this->themeChangedEvent();
    this->fetchPrediction();
}

void ManagePredictionDialog::buildUi()
{
}

void ManagePredictionDialog::fetchPrediction()
{
    const auto roomId = this->channel_->roomId();
    getHelix()->getPredictions(
        roomId, {}, 1, {},
        [this](const auto &result) {
            if (result.predictions.empty())
            {
                this->statusLabel_->setText("No active prediction in this channel.");
                return;
            }

            auto prediction = result.predictions.front();
            this->currentPredictionId_ = prediction.id;

            if (prediction.status != "ACTIVE" && prediction.status != "LOCKED")
            {
                this->statusLabel_->setText("The latest prediction is already " + prediction.status);
                return;
            }

            this->statusLabel_->hide();

            // Clear previous content
            QLayoutItem *item;
            if (this->contentWidget_->layout()) {
                while ((item = this->contentWidget_->layout()->takeAt(0)) != nullptr) {
                    delete item->widget();
                    delete item;
                }
                delete this->contentWidget_->layout();
            }

            auto *layout = new QVBoxLayout(this->contentWidget_);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(12);

            auto *titleLabel = new QLabel(prediction.title, this);
            titleLabel->setWordWrap(true);
            titleLabel->setStyleSheet("font-size: 18px; font-weight: bold;");
            layout->addWidget(titleLabel);

            auto *statusBox = new QHBoxLayout();
            auto *statusText = new QLabel(QString("Status: %1").arg(prediction.status), this);
            statusText->setStyleSheet("font-weight: bold; color: " + QString(prediction.status == "ACTIVE" ? "#00FF00" : "#FFA500") + ";");
            statusBox->addWidget(statusText);
            statusBox->addStretch(1);
            layout->addLayout(statusBox);

            int totalPoints = 0;
            int totalUsers = 0;
            for (const auto &outcome : prediction.outcomes) {
                totalPoints += outcome.channelPoints;
                totalUsers += outcome.users;
            }

            auto *statsLabel = new QLabel(QString("Total Pool: %1 points | %2 users").arg(localizeNumbers(totalPoints), localizeNumbers(totalUsers)), this);
            layout->addWidget(statsLabel);

            // Outcomes
            for (const auto &outcome : prediction.outcomes) {
                auto *outcomeBox = new QWidget(this);
                outcomeBox->setObjectName("outcomeBox");
                auto *outcomeLayout = new QVBoxLayout(outcomeBox);
                
                int pct = totalPoints > 0 ? qRound(100.0 * outcome.channelPoints / totalPoints) : 0;
                
                auto *oTitle = new QLabel(QString("%1 (%2%)").arg(outcome.title, QString::number(pct)), this);
                oTitle->setStyleSheet("font-weight: bold; font-size: 14px;");
                outcomeLayout->addWidget(oTitle);

                auto *oStats = new QLabel(QString("%1 points | %2 users").arg(localizeNumbers(outcome.channelPoints), localizeNumbers(outcome.users)), this);
                outcomeLayout->addWidget(oStats);

                auto *resolveBtn = new QPushButton("Choose Winner", this);
                resolveBtn->setObjectName("resolveBtn");
                resolveBtn->setCursor(Qt::PointingHandCursor);
                if (prediction.status == "ACTIVE") {
                    resolveBtn->setEnabled(false);
                    resolveBtn->setText("Must Lock First");
                }
                QObject::connect(resolveBtn, &QPushButton::clicked, [this, id = outcome.id]() {
                    this->completePrediction(id);
                });
                outcomeLayout->addWidget(resolveBtn);
                
                layout->addWidget(outcomeBox);
            }

            // Actions
            auto *actionBox = new QHBoxLayout();
            
            auto *cancelBtn = new QPushButton("Cancel / Return Points", this);
            cancelBtn->setObjectName("cancelBtn");
            cancelBtn->setCursor(Qt::PointingHandCursor);
            QObject::connect(cancelBtn, &QPushButton::clicked, this, &ManagePredictionDialog::cancelPrediction);
            actionBox->addWidget(cancelBtn);

            actionBox->addStretch(1);

            if (prediction.status == "ACTIVE") {
                auto *lockBtn = new QPushButton("Lock Prediction", this);
                lockBtn->setObjectName("lockBtn");
                lockBtn->setCursor(Qt::PointingHandCursor);
                QObject::connect(lockBtn, &QPushButton::clicked, this, &ManagePredictionDialog::lockPrediction);
                actionBox->addWidget(lockBtn);
            }

            layout->addLayout(actionBox);
            
            this->themeChangedEvent();
        },
        [this](const QString &error) {
            this->statusLabel_->setText("Failed to fetch prediction: " + error);
        });
}

void ManagePredictionDialog::lockPrediction()
{
    this->channel_->lockPredictionEvent(
        this->currentPredictionId_,
        [this]() {
            this->channel_->addSystemMessage("Locked prediction successfully.");
            this->fetchPrediction(); // Refresh UI
        },
        [this](const QString &error) {
            QMessageBox::critical(this, "Error", "Failed to lock prediction: " + error);
        });
}

void ManagePredictionDialog::cancelPrediction()
{
    this->channel_->cancelPredictionEvent(
        this->currentPredictionId_,
        [this]() {
            this->channel_->addSystemMessage("Canceled prediction and refunded points.");
            this->close();
        },
        [this](const QString &error) {
            QMessageBox::critical(this, "Error", "Failed to cancel prediction: " + error);
        });
}

void ManagePredictionDialog::completePrediction(const QString &outcomeId)
{
    this->channel_->resolvePredictionEvent(
        this->currentPredictionId_, outcomeId,
        [this]() {
            this->channel_->addSystemMessage("Completed prediction successfully.");
            this->close();
        },
        [this](const QString &error) {
            QMessageBox::critical(this, "Error", "Failed to complete prediction: " + error);
        });
}

void ManagePredictionDialog::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();

    auto bg = this->theme->window.background.name();
    auto fg = this->theme->messages.textColors.regular.name();
    auto accent = this->theme->accent.name();
    auto accentHover = this->theme->accent.lighter(120).name();
    
    this->setStyleSheet(QString(R"(
        QWidget {
            color: %1;
            background-color: %2;
        }
        QWidget#outcomeBox {
            background-color: rgba(255, 255, 255, 0.05);
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 6px;
            padding: 10px;
        }
        QPushButton#resolveBtn {
            background-color: %3;
            color: white;
            border: none;
            border-radius: 4px;
            padding: 6px 12px;
            font-weight: bold;
            margin-top: 5px;
        }
        QPushButton#resolveBtn:hover {
            background-color: %4;
        }
        QPushButton#resolveBtn:disabled {
            background-color: #555;
            color: #888;
        }
        QPushButton#lockBtn {
            background-color: %3;
            color: white;
            border: none;
            border-radius: 4px;
            padding: 8px 16px;
            font-weight: bold;
        }
        QPushButton#lockBtn:hover {
            background-color: %4;
        }
        QPushButton#cancelBtn {
            background-color: transparent;
            color: #ff4444;
            border: 1px solid #ff4444;
            border-radius: 4px;
            padding: 8px 16px;
            font-weight: bold;
        }
        QPushButton#cancelBtn:hover {
            background-color: rgba(255, 68, 68, 0.1);
        }
    )").arg(fg, bg, accent, accentHover));
}

}  // namespace chatterino
