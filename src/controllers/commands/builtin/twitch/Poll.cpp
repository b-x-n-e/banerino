// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/twitch/Poll.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "controllers/commands/common/ChannelAction.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/dialogs/PollDialog.hpp"
#include "widgets/Window.hpp"

#include <chrono>

namespace {

using namespace chatterino;

constexpr auto MIN_POLL_DURATION = std::chrono::seconds(10);
constexpr auto MAX_POLL_DURATION = std::chrono::seconds(1800);

}  // namespace

namespace chatterino::commands {

QString createPoll(const CommandContext &ctx)
{
    if (ctx.words.length() == 1)
    {
        if (ctx.twitchChannel == nullptr)
        {
            if (ctx.channel != nullptr)
            {
                ctx.channel->addSystemMessage(
                    "The /poll command only works in Twitch channels");
            }
            return "";
        }

        auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
        if (currentUser->isAnon())
        {
            ctx.channel->addSystemMessage(
                "You must be logged in to create a poll!");
            return "";
        }

        auto *dialog = new PollDialog(
            ctx.twitchChannel, getApp()->getWindows()->getMainWindow().window());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
        return "";
    }

    const auto command = QStringLiteral("/poll");
    const auto usage = QStringLiteral(
        R"(Usage: "/poll --title "<title>" --duration <duration>[time unit] --choice "<choice1>" --choice "<choice2>" [options...]" - Creates a poll for users to vote among the defined options. Title may not exceed 60 characters. There must be between two and five poll choices. Duration must be a positive integer; time unit (optional, default=s) must be one of s, m; maximum duration is 30 minutes. Options: --points <points> to allow spending the specified channel points for each additional vote.)");
    const auto action = parseUserParticipationAction(
        ctx, command, usage, MIN_POLL_DURATION, MAX_POLL_DURATION);

    if (!action.has_value())
    {
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(action.error());
        }
        else
        {
            qCWarning(chatterinoCommands)
                << "Error parsing command:" << action.error();
        }
        return "";
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        ctx.channel->addSystemMessage(
            "You must be logged in to create a poll!");
        return "";
    }

    const auto &poll = action.value();
    getHelix()->createPoll(
        poll.broadcasterID, poll.title, poll.choices, poll.duration,
        poll.pointsPerVote,
        [channel = ctx.channel, poll] {
            channel->addSystemMessage(
                QString("Created poll: '%1'").arg(poll.title));
        },
        [channel = ctx.channel](const auto &error) {
            channel->addSystemMessage("Failed to create poll - " + error);
        });

    return "";
}

QString endPoll(const CommandContext &ctx)
{
    if (ctx.twitchChannel == nullptr)
    {
        const auto err = QStringLiteral(
            "The /endpoll command only works in Twitch channels");
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(err);
        }
        else
        {
            qCWarning(chatterinoCommands) << "Invalid command context:" << err;
        }
        return "";
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        ctx.channel->addSystemMessage("You must be logged in to end a poll!");
        return "";
    }

    ctx.twitchChannel->getActivePoll(
        [channel = ctx.channel, tc = ctx.twitchChannel](std::optional<HelixPoll> result) {
            if (!result.has_value())
            {
                channel->addSystemMessage("Failed to find any polls");
                return;
            }

            auto poll = result.value();
            if (poll.status != "ACTIVE")
            {
                channel->addSystemMessage("Could not find an active poll");
                return;
            }

            tc->terminatePoll(
                poll.id,
                [channel, poll]() {
                    channel->addSystemMessage(
                        QString("Ended poll: '%1'").arg(poll.title));
                },
                [channel](const QString &error) {
                    channel->addSystemMessage("Failed to end the poll - " +
                                              error);
                });
        },
        [channel = ctx.channel](const QString &error) {
            channel->addSystemMessage("Failed to query polls - " + error);
        });

    return "";
}

QString cancelPoll(const CommandContext &ctx)
{
    if (ctx.twitchChannel == nullptr)
    {
        const auto err = QStringLiteral(
            "The /cancelpoll command only works in Twitch channels");
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(err);
        }
        else
        {
            qCWarning(chatterinoCommands) << "Invalid command context:" << err;
        }
        return "";
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        ctx.channel->addSystemMessage(
            "You must be logged in to cancel a poll!");
        return "";
    }

    ctx.twitchChannel->getActivePoll(
        [channel = ctx.channel, tc = ctx.twitchChannel](std::optional<HelixPoll> result) {
            if (!result.has_value())
            {
                channel->addSystemMessage("Failed to find any polls");
                return;
            }

            auto poll = result.value();
            if (poll.status != "ACTIVE")
            {
                channel->addSystemMessage("Could not find an active poll");
                return;
            }

            tc->archivePoll(
                poll.id,
                [channel, poll]() {
                    channel->addSystemMessage(
                        QString("Canceled poll: '%1'").arg(poll.title));
                },
                [channel](const QString &error) {
                    channel->addSystemMessage("Failed to cancel the poll - " +
                                              error);
                });
        },
        [channel = ctx.channel](const QString &error) {
            channel->addSystemMessage("Failed to query polls - " + error);
        });

    return "";
}

}  // namespace chatterino::commands
