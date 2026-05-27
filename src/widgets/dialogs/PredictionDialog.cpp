// SPDX-FileCopyrightText: 2024 Contributors to Banerino
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/PredictionDialog.hpp"

#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "singletons/Theme.hpp"

#include <QMessageBox>

namespace chatterino {

PredictionDialog::PredictionDialog(TwitchChannel *channel, QWidget *parent)
    : BaseWindow({BaseWindow::Flags::Dialog}, parent)
    , channel_(channel)
{
    this->setWindowTitle("Create New Prediction");
    this->resize(450, 550);

    this->buildUi();

    // Start with 2 empty choices/outcomes (minimum required by Twitch)
    this->addChoiceInput();
    this->addChoiceInput();
    
    this->themeChangedEvent();
}

void PredictionDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this->getLayoutContainer());
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(16);

    // Title / Question
    auto *titleLabel = new QLabel("Prediction Title", this);
    titleLabel->setStyleSheet("font-weight: bold; font-size: 14px;");
    layout->addWidget(titleLabel);

    this->titleInput_ = new QLineEdit(this);
    this->titleInput_->setPlaceholderText("Name your prediction (max 45 characters)");
    this->titleInput_->setMaxLength(45);
    this->titleInput_->setObjectName("predictInput");
    layout->addWidget(this->titleInput_);

    // Choices
    auto *choicesLabel = new QLabel("Outcomes", this);
    choicesLabel->setStyleSheet("font-weight: bold; font-size: 14px;");
    layout->addWidget(choicesLabel);

    this->choicesLayout_ = new QVBoxLayout();
    this->choicesLayout_->setSpacing(8);
    layout->addLayout(this->choicesLayout_);

    this->addChoiceButton_ = new QPushButton("+ Add Option", this);
    this->addChoiceButton_->setObjectName("addChoiceBtn");
    this->addChoiceButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->addChoiceButton_, &QPushButton::clicked, this, &PredictionDialog::addChoiceInput);
    layout->addWidget(this->addChoiceButton_);

    // Settings
    auto *settingsLabel = new QLabel("Settings", this);
    settingsLabel->setStyleSheet("font-weight: bold; font-size: 14px; margin-top: 10px;");
    layout->addWidget(settingsLabel);

    // Duration
    auto *durationLayout = new QHBoxLayout();
    auto *durationText = new QLabel("Duration", this);
    durationLayout->addWidget(durationText);
    
    this->durationSelect_ = new QComboBox(this);
    this->durationSelect_->addItem("30 Seconds", 30);
    this->durationSelect_->addItem("1 Minute", 60);
    this->durationSelect_->addItem("2 Minutes", 120);
    this->durationSelect_->addItem("5 Minutes", 300);
    this->durationSelect_->addItem("10 Minutes", 600);
    this->durationSelect_->addItem("15 Minutes", 900);
    this->durationSelect_->addItem("30 Minutes", 1800);
    this->durationSelect_->setCurrentIndex(2); // Default 2 minutes
    this->durationSelect_->setObjectName("predictCombo");
    durationLayout->addWidget(this->durationSelect_);
    durationLayout->addStretch(1);
    layout->addLayout(durationLayout);

    // Error Label
    this->errorLabel_ = new QLabel("", this);
    this->errorLabel_->setStyleSheet("color: #ff4444; font-weight: bold;");
    this->errorLabel_->hide();
    layout->addWidget(this->errorLabel_);

    layout->addStretch(1);

    // Buttons
    auto *buttonBox = new QHBoxLayout();
    this->cancelButton_ = new QPushButton("Cancel", this);
    this->cancelButton_->setObjectName("cancelBtn");
    this->cancelButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->cancelButton_, &QPushButton::clicked, this, &PredictionDialog::close);
    buttonBox->addWidget(this->cancelButton_);

    this->cancelActiveButton_ = new QPushButton("Cancel Active / Return Points", this);
    this->cancelActiveButton_->setObjectName("cancelActiveBtn");
    this->cancelActiveButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->cancelActiveButton_, &QPushButton::clicked, this, &PredictionDialog::cancelActivePrediction);
    buttonBox->addWidget(this->cancelActiveButton_);

    buttonBox->addStretch(1);

    this->startButton_ = new QPushButton("Start Prediction", this);
    this->startButton_->setObjectName("startBtn");
    this->startButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->startButton_, &QPushButton::clicked, this, &PredictionDialog::startPrediction);
    buttonBox->addWidget(this->startButton_);

    layout->addLayout(buttonBox);
}

void PredictionDialog::addChoiceInput()
{
    if (this->choices_.size() >= 10) {
        return; // Twitch supports up to 10 choices for predictions
    }

    ChoiceInput choice;
    choice.container = new QWidget(this);
    auto *layout = new QHBoxLayout(choice.container);
    layout->setContentsMargins(0, 0, 0, 0);

    choice.input = new QLineEdit(this);
    choice.input->setPlaceholderText(QString("Option %1 (max 25 characters)").arg(this->choices_.size() + 1));
    choice.input->setMaxLength(25);
    choice.input->setObjectName("predictInput");
    layout->addWidget(choice.input, 1);

    choice.removeButton = new QPushButton("✕", this);
    choice.removeButton->setObjectName("removeBtn");
    choice.removeButton->setFixedSize(28, 28);
    choice.removeButton->setCursor(Qt::PointingHandCursor);
    choice.removeButton->setToolTip("Remove Option");
    
    QObject::connect(choice.removeButton, &QPushButton::clicked, [this, choicePtr = choice.container]() {
        // Find the index of this container and remove it
        for (size_t i = 0; i < this->choices_.size(); ++i) {
            if (this->choices_[i].container == choicePtr) {
                this->removeChoiceInput(i);
                break;
            }
        }
    });
    
    layout->addWidget(choice.removeButton);

    this->choicesLayout_->addWidget(choice.container);
    this->choices_.push_back(choice);

    this->updateChoiceButtons();
    this->themeChangedEvent();
}

void PredictionDialog::removeChoiceInput(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= this->choices_.size()) {
        return;
    }
    
    if (this->choices_.size() <= 2) {
        return; // Minimum 2 options
    }

    auto &choice = this->choices_[index];
    this->choicesLayout_->removeWidget(choice.container);
    choice.container->deleteLater();
    this->choices_.erase(this->choices_.begin() + index);

    // Update placeholders
    for (size_t i = 0; i < this->choices_.size(); ++i) {
        this->choices_[i].input->setPlaceholderText(QString("Option %1 (max 25 characters)").arg(i + 1));
    }

    this->updateChoiceButtons();
}

void PredictionDialog::updateChoiceButtons()
{
    // Minimum 2 options, hide remove buttons if only 2 left
    bool canRemove = this->choices_.size() > 2;
    for (auto &choice : this->choices_) {
        choice.removeButton->setVisible(canRemove);
    }

    // Maximum 10 options, hide add button if at max
    this->addChoiceButton_->setVisible(this->choices_.size() < 10);
}

void PredictionDialog::startPrediction()
{
    this->errorLabel_->hide();

    // Validate Title
    QString title = this->titleInput_->text().trimmed();
    if (title.isEmpty()) {
        this->errorLabel_->setText("Prediction title cannot be empty.");
        this->errorLabel_->show();
        this->titleInput_->setFocus();
        return;
    }

    // Validate Choices
    QStringList choiceStrings;
    for (const auto &choice : this->choices_) {
        QString text = choice.input->text().trimmed();
        if (text.isEmpty()) {
            this->errorLabel_->setText("All prediction options must be filled out.");
            this->errorLabel_->show();
            choice.input->setFocus();
            return;
        }
        choiceStrings.append(text);
    }

    // Get Duration
    int durationSecs = this->durationSelect_->currentData().toInt();

    // Disable UI while submitting
    this->startButton_->setEnabled(false);
    this->startButton_->setText("Starting...");

    this->channel_->createPredictionEvent(
        title, choiceStrings, durationSecs,
        [this, title]() {
            this->channel_->addSystemMessage(QString("Created prediction: '%1'").arg(title));
            this->close();
        },
        [this](const QString &error) {
            this->startButton_->setEnabled(true);
            this->startButton_->setText("Start Prediction");
            this->errorLabel_->setText("Failed to create prediction: " + error);
            this->errorLabel_->show();
        });
}

void PredictionDialog::cancelActivePrediction()
{
    this->errorLabel_->hide();
    this->cancelActiveButton_->setEnabled(false);
    this->cancelActiveButton_->setText("Canceling...");

    const auto roomId = this->channel_->roomId();
    getHelix()->getPredictions(
        roomId, {}, 1, {},
        [this, roomId](const auto &result) {
            if (result.predictions.empty())
            {
                this->errorLabel_->setText("Failed to find any active predictions");
                this->errorLabel_->show();
                this->cancelActiveButton_->setEnabled(true);
                this->cancelActiveButton_->setText("Cancel Active / Return Points");
                return;
            }

            auto prediction = result.predictions.front();
            if (prediction.status != "ACTIVE" && prediction.status != "LOCKED")
            {
                this->errorLabel_->setText("Could not find an open prediction to cancel");
                this->errorLabel_->show();
                this->cancelActiveButton_->setEnabled(true);
                this->cancelActiveButton_->setText("Cancel Active / Return Points");
                return;
            }

            this->channel_->cancelPredictionEvent(
                prediction.id,
                [this, prediction]() {
                    int totalPoints = 0;
                    int numUsers = 0;
                    for (const auto &outcome : prediction.outcomes)
                    {
                        totalPoints += outcome.channelPoints;
                        numUsers += outcome.users;
                    }

                    this->channel_->addSystemMessage(
                        QString("Refunded %1 points to %2 users for "
                                "prediction: '%3'")
                            .arg(localizeNumbers(totalPoints),
                                 localizeNumbers(numUsers), prediction.title));
                    this->close();
                },
                [this](const QString &error) {
                    this->errorLabel_->setText("Failed to cancel prediction: " + error);
                    this->errorLabel_->show();
                    this->cancelActiveButton_->setEnabled(true);
                    this->cancelActiveButton_->setText("Cancel Active / Return Points");
                });
        },
        [this](const QString &error) {
            this->errorLabel_->setText("Failed to fetch predictions: " + error);
            this->errorLabel_->show();
            this->cancelActiveButton_->setEnabled(true);
            this->cancelActiveButton_->setText("Cancel Active / Return Points");
        });
}

void PredictionDialog::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();

    auto bg = this->theme->window.background.name();
    auto fg = this->theme->messages.textColors.regular.name();
    auto inputBg = this->theme->splits.input.background.name();
    auto inputBorder = this->theme->splits.header.border.name();
    auto accent = this->theme->accent.name();
    auto accentHover = this->theme->accent.lighter(120).name();
    
    this->setStyleSheet(QString(R"(
        QWidget {
            color: %1;
            background-color: %2;
        }
        QLineEdit#predictInput {
            background-color: %3;
            border: 1px solid %4;
            border-radius: 4px;
            padding: 8px;
            font-size: 13px;
        }
        QLineEdit#predictInput:focus {
            border: 1px solid %5;
            background-color: %2;
        }
        QComboBox#predictCombo {
            background-color: %3;
            border: 1px solid %4;
            border-radius: 4px;
            padding: 6px;
            min-width: 120px;
        }
        QComboBox#predictCombo::drop-down {
            border: none;
        }
        QPushButton#addChoiceBtn {
            background-color: transparent;
            color: %5;
            border: none;
            text-align: left;
            font-weight: bold;
            padding: 4px 0;
        }
        QPushButton#addChoiceBtn:hover {
            color: %6;
        }
        QPushButton#removeBtn {
            background-color: transparent;
            color: #ff4444;
            border: none;
            font-size: 16px;
            font-weight: bold;
        }
        QPushButton#removeBtn:hover {
            background-color: rgba(255, 68, 68, 0.1);
            border-radius: 4px;
        }
        QPushButton#startBtn {
            background-color: %5;
            color: white;
            border: none;
            border-radius: 4px;
            padding: 8px 16px;
            font-weight: bold;
        }
        QPushButton#startBtn:hover {
            background-color: %6;
        }
        QPushButton#startBtn:disabled {
            background-color: #555;
            color: #888;
        }
        QPushButton#cancelBtn {
            background-color: rgba(255, 255, 255, 0.1);
            color: %1;
            border: none;
            border-radius: 4px;
            padding: 8px 16px;
            font-weight: bold;
        }
        QPushButton#cancelBtn:hover {
            background-color: rgba(255, 255, 255, 0.15);
        }
        QPushButton#cancelActiveBtn {
            background-color: transparent;
            color: #ff4444;
            border: 1px solid #ff4444;
            border-radius: 4px;
            padding: 8px 16px;
            font-weight: bold;
        }
        QPushButton#cancelActiveBtn:hover {
            background-color: rgba(255, 68, 68, 0.1);
        }
        QPushButton#cancelActiveBtn:disabled {
            color: #888;
            border-color: #555;
        }
    )").arg(fg, bg, inputBg, inputBorder, accent, accentHover));
}

}  // namespace chatterino

