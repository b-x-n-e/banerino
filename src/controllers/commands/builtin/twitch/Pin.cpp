// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/twitch/Pin.hpp"

#include "controllers/commands/CommandContext.hpp"
#include "providers/twitch/TwitchChannel.hpp"

namespace chatterino::commands {

QString pinMessage(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /pin command only works in Twitch channels.");
        return "";
    }

    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage("Usage: /pin <msg-id> - Pins the "
                                      "specified message.");
        return "";
    }

    auto messageID = ctx.words.at(1);
    ctx.twitchChannel->pinMessage(messageID);

    return "";
}

QString unpinMessage(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /unpin command only works in Twitch channels.");
        return "";
    }

    ctx.twitchChannel->unpinMessage();

    return "";
}

}  // namespace chatterino::commands
