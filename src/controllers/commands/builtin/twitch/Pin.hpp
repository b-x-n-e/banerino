// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

namespace chatterino {

struct CommandContext;

namespace commands {

    QString pinMessage(const CommandContext &ctx);
    QString unpinMessage(const CommandContext &ctx);

}  // namespace commands
}  // namespace chatterino
