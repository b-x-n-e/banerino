// SPDX-FileCopyrightText: 2024 Contributors to Banerino
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/PollDialog.hpp"

#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "singletons/Theme.hpp"

#include <QIntValidator>
#include <QMessageBox>

namespace chatterino {

PollDialog::PollDialog(TwitchChannel *channel, QWidget *parent)
    : BaseWindow({BaseWindow::Flags::Dialog}, parent)
    , channel_(channel)
{
    this->setWindowTitle("Create New Poll");
    this->resize(450, 500);

    this->buildUi();

    // Start with 2 empty choices (the minimum required by Twitch)
    this->addChoiceInput();
    this->addChoiceInput();
    
    this->themeChangedEvent();
}

void PollDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this->getLayoutContainer());
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(16);

    // Question / Title
    auto *titleLabel = new QLabel("Question", this);
    titleLabel->setStyleSheet("font-weight: bold; font-size: 14px;");
    layout->addWidget(titleLabel);

    this->titleInput_ = new QLineEdit(this);
    this->titleInput_->setPlaceholderText("Ask a question (max 60 characters)");
    this->titleInput_->setMaxLength(60);
    this->titleInput_->setObjectName("pollInput");
    layout->addWidget(this->titleInput_);

    // Choices
    auto *choicesLabel = new QLabel("Responses", this);
    choicesLabel->setStyleSheet("font-weight: bold; font-size: 14px;");
    layout->addWidget(choicesLabel);

    this->choicesLayout_ = new QVBoxLayout();
    this->choicesLayout_->setSpacing(8);
    layout->addLayout(this->choicesLayout_);

    this->addChoiceButton_ = new QPushButton("+ Add Choice", this);
    this->addChoiceButton_->setObjectName("addChoiceBtn");
    this->addChoiceButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->addChoiceButton_, &QPushButton::clicked, this, &PollDialog::addChoiceInput);
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
    this->durationSelect_->addItem("1 Minute", 60);
    this->durationSelect_->addItem("2 Minutes", 120);
    this->durationSelect_->addItem("3 Minutes", 180);
    this->durationSelect_->addItem("5 Minutes", 300);
    this->durationSelect_->addItem("10 Minutes", 600);
    this->durationSelect_->addItem("15 Minutes", 900);
    this->durationSelect_->addItem("30 Minutes", 1800);
    this->durationSelect_->setCurrentIndex(3); // Default 5 minutes
    this->durationSelect_->setObjectName("pollCombo");
    durationLayout->addWidget(this->durationSelect_);
    durationLayout->addStretch(1);
    layout->addLayout(durationLayout);

    // Channel Points Voting
    this->allowAdditionalVotesCheck_ = new QCheckBox("Allow additional votes using Channel Points", this);
    layout->addWidget(this->allowAdditionalVotesCheck_);

    this->pointsPerVoteContainer_ = new QWidget(this);
    auto *pointsLayout = new QHBoxLayout(this->pointsPerVoteContainer_);
    pointsLayout->setContentsMargins(20, 0, 0, 0); // Indent
    auto *pointsLabel = new QLabel("Points per additional vote:", this);
    pointsLayout->addWidget(pointsLabel);
    
    this->pointsPerVoteInput_ = new QLineEdit(this);
    this->pointsPerVoteInput_->setValidator(new QIntValidator(1, 1000000, this));
    this->pointsPerVoteInput_->setText("10");
    this->pointsPerVoteInput_->setFixedWidth(100);
    this->pointsPerVoteInput_->setObjectName("pollInput");
    pointsLayout->addWidget(this->pointsPerVoteInput_);
    pointsLayout->addStretch(1);
    
    layout->addWidget(this->pointsPerVoteContainer_);
    this->pointsPerVoteContainer_->hide(); // Hidden by default

    QObject::connect(this->allowAdditionalVotesCheck_, &QCheckBox::toggled, [this](bool checked) {
        this->pointsPerVoteContainer_->setVisible(checked);
    });

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
    QObject::connect(this->cancelButton_, &QPushButton::clicked, this, &PollDialog::close);
    buttonBox->addWidget(this->cancelButton_);

    buttonBox->addStretch(1);

    this->startButton_ = new QPushButton("Start Poll", this);
    this->startButton_->setObjectName("startBtn");
    this->startButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->startButton_, &QPushButton::clicked, this, &PollDialog::startPoll);
    buttonBox->addWidget(this->startButton_);

    layout->addLayout(buttonBox);
}

void PollDialog::addChoiceInput()
{
    if (this->choices_.size() >= 5) {
        return;
    }

    ChoiceInput choice;
    choice.container = new QWidget(this);
    auto *layout = new QHBoxLayout(choice.container);
    layout->setContentsMargins(0, 0, 0, 0);

    choice.input = new QLineEdit(this);
    choice.input->setPlaceholderText(QString("Choice %1 (max 25 characters)").arg(this->choices_.size() + 1));
    choice.input->setMaxLength(25);
    choice.input->setObjectName("pollInput");
    layout->addWidget(choice.input, 1);

    choice.removeButton = new QPushButton("✕", this);
    choice.removeButton->setObjectName("removeBtn");
    choice.removeButton->setFixedSize(28, 28);
    choice.removeButton->setCursor(Qt::PointingHandCursor);
    choice.removeButton->setToolTip("Remove choice");
    
    int index = this->choices_.size();
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

void PollDialog::removeChoiceInput(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= this->choices_.size()) {
        return;
    }
    
    if (this->choices_.size() <= 2) {
        return; // Minimum 2 choices
    }

    auto &choice = this->choices_[index];
    this->choicesLayout_->removeWidget(choice.container);
    choice.container->deleteLater();
    this->choices_.erase(this->choices_.begin() + index);

    // Update placeholders
    for (size_t i = 0; i < this->choices_.size(); ++i) {
        this->choices_[i].input->setPlaceholderText(QString("Choice %1 (max 25 characters)").arg(i + 1));
    }

    this->updateChoiceButtons();
}

void PollDialog::updateChoiceButtons()
{
    // Minimum 2 choices, hide remove buttons if only 2 left
    bool canRemove = this->choices_.size() > 2;
    for (auto &choice : this->choices_) {
        choice.removeButton->setVisible(canRemove);
    }

    // Maximum 5 choices, hide add button if at max
    this->addChoiceButton_->setVisible(this->choices_.size() < 5);
}

void PollDialog::startPoll()
{
    this->errorLabel_->hide();

    // Validate Title
    QString title = this->titleInput_->text().trimmed();
    if (title.isEmpty()) {
        this->errorLabel_->setText("Question title cannot be empty.");
        this->errorLabel_->show();
        this->titleInput_->setFocus();
        return;
    }

    // Validate Choices
    QStringList choiceStrings;
    for (const auto &choice : this->choices_) {
        QString text = choice.input->text().trimmed();
        if (text.isEmpty()) {
            this->errorLabel_->setText("All poll choices must be filled out.");
            this->errorLabel_->show();
            choice.input->setFocus();
            return;
        }
        choiceStrings.append(text);
    }

    // Validate Points
    int pointsPerVote = 0;
    if (this->allowAdditionalVotesCheck_->isChecked()) {
        pointsPerVote = this->pointsPerVoteInput_->text().toInt();
        if (pointsPerVote <= 0) {
            this->errorLabel_->setText("Points per vote must be greater than 0.");
            this->errorLabel_->show();
            this->pointsPerVoteInput_->setFocus();
            return;
        }
    }

    // Get Duration
    int durationSecs = this->durationSelect_->currentData().toInt();

    // Disable UI while submitting
    this->startButton_->setEnabled(false);
    this->startButton_->setText("Starting...");

    this->channel_->createPoll(
        title, choiceStrings, durationSecs,
        pointsPerVote,
        [this, title]() {
            this->channel_->addSystemMessage(QString("Created poll: '%1'").arg(title));
            this->close();
        },
        [this](const QString &error) {
            this->startButton_->setEnabled(true);
            this->startButton_->setText("Start Poll");
            this->errorLabel_->setText("Failed to create poll: " + error);
            this->errorLabel_->show();
        });
}

void PollDialog::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();

    auto bg = this->theme->window.background.name();
    auto fg = this->theme->messages.textColors.regular.name();
    auto inputBg = this->theme->splits.input.background.name();
    auto inputBorder = this->theme->splits.header.border.name();
    auto accent = this->theme->accent.name();
    auto accentHover = this->theme->accent.lighter(120).name();
    
    // Premium Twitch-like styling
    this->setStyleSheet(QString(R"(
        QWidget {
            color: %1;
            background-color: %2;
        }
        QLineEdit#pollInput {
            background-color: %3;
            border: 1px solid %4;
            border-radius: 4px;
            padding: 8px;
            font-size: 13px;
        }
        QLineEdit#pollInput:focus {
            border: 1px solid %5;
            background-color: %2;
        }
        QComboBox#pollCombo {
            background-color: %3;
            border: 1px solid %4;
            border-radius: 4px;
            padding: 6px;
            min-width: 120px;
        }
        QComboBox#pollCombo::drop-down {
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
    )").arg(fg, bg, inputBg, inputBorder, accent, accentHover));
}

}  // namespace chatterino
