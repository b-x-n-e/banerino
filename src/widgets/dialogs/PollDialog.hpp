// SPDX-FileCopyrightText: 2024 Contributors to Banerino
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWindow.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <vector>

namespace chatterino {

class TwitchChannel;

class PollDialog : public BaseWindow
{
    Q_OBJECT

public:
    explicit PollDialog(TwitchChannel *channel, QWidget *parent = nullptr);

protected:
    void themeChangedEvent() override;

private:
    void buildUi();
    void addChoiceInput();
    void removeChoiceInput(int index);
    void updateChoiceButtons();
    void startPoll();

    TwitchChannel *channel_;

    // UI elements
    QLineEdit *titleInput_ = nullptr;
    
    struct ChoiceInput {
        QWidget *container;
        QLineEdit *input;
        QPushButton *removeButton;
    };
    std::vector<ChoiceInput> choices_;
    QVBoxLayout *choicesLayout_ = nullptr;
    QPushButton *addChoiceButton_ = nullptr;

    QComboBox *durationSelect_ = nullptr;
    QCheckBox *allowAdditionalVotesCheck_ = nullptr;
    QWidget *pointsPerVoteContainer_ = nullptr;
    QLineEdit *pointsPerVoteInput_ = nullptr;

    QPushButton *startButton_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QLabel *errorLabel_ = nullptr;
};

}  // namespace chatterino
