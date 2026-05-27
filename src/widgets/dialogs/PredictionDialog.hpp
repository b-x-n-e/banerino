// SPDX-FileCopyrightText: 2024 Contributors to Banerino
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWindow.hpp"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <vector>

namespace chatterino {

class TwitchChannel;

class PredictionDialog : public BaseWindow
{
    Q_OBJECT

public:
    explicit PredictionDialog(TwitchChannel *channel, QWidget *parent = nullptr);

protected:
    void themeChangedEvent() override;

private:
    void buildUi();
    void addChoiceInput();
    void removeChoiceInput(int index);
    void updateChoiceButtons();
    void startPrediction();
    void cancelActivePrediction();

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

    QPushButton *startButton_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QPushButton *cancelActiveButton_ = nullptr;
    QLabel *errorLabel_ = nullptr;
};

}  // namespace chatterino
