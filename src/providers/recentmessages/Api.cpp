// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/recentmessages/Api.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "providers/recentmessages/Impl.hpp"
#include "util/PostToThread.hpp"
#include <QTimer>
#include <functional>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
const auto &LOG = chatterinoRecentMessages;

}  // namespace

namespace chatterino::recentmessages {

using namespace recentmessages::detail;

void load(
    const QString &channelName, std::weak_ptr<Channel> channelPtr,
    ResultCallback onLoaded, ErrorCallback onError, const int limit,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        after,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>
        before,
    const bool jitter)
{
    qCDebug(LOG) << "Loading recent messages for" << channelName;

    const auto url =
        constructRecentMessagesUrl(channelName, limit, after, before);

    const long delayMs = jitter ? std::rand() % 100 : 0;
    QTimer::singleShot(delayMs, [=] {
        if (isAppAboutToQuit())
        {
            return;
        }

        NetworkRequest(url)
            .onSuccess([channelPtr, onLoaded](const auto &result) {
                assert(!isAppAboutToQuit());

                auto shared = channelPtr.lock();
                if (!shared)
                {
                    return;
                }

                qCDebug(LOG) << "Successfully loaded recent messages for"
                             << shared->getName();

                auto root = result.parseJson();
                auto parsedMessages = parseRecentMessages(root);

                // Use weak pointer for the async loop to safely handle deletion/parting of channel
                auto weak = std::weak_ptr<Channel>(shared);

                struct ParseState {
                    std::vector<Communi::IrcMessage *> parsedMessages;
                    std::vector<MessagePtr> builtMessages;
                    size_t index = 0;
                    QJsonObject root;
                };

                auto state = std::make_shared<ParseState>();
                state->parsedMessages = std::move(parsedMessages);
                state->root = std::move(root);

                auto processChunk = std::make_shared<std::function<void()>>();
                *processChunk = [state, weak, onLoaded, processChunk]() {
                    if (isAppAboutToQuit())
                    {
                        for (auto *msg : state->parsedMessages)
                        {
                            msg->deleteLater();
                        }
                        return;
                    }

                    auto sharedChannel = weak.lock();
                    if (!sharedChannel)
                    {
                        for (auto *msg : state->parsedMessages)
                        {
                            msg->deleteLater();
                        }
                        return;
                    }

                    // Process messages in chunks of 50 to prevent freezing the GUI thread
                    size_t chunkSize = 50;
                    size_t end = std::min(state->index + chunkSize, state->parsedMessages.size());

                    std::vector<Communi::IrcMessage *> chunk(
                        state->parsedMessages.begin() + state->index,
                        state->parsedMessages.begin() + end
                    );

                    auto chunkBuilt = buildRecentMessages(chunk, sharedChannel.get());

                    state->builtMessages.insert(
                        state->builtMessages.end(),
                        std::make_move_iterator(chunkBuilt.begin()),
                        std::make_move_iterator(chunkBuilt.end())
                    );

                    state->index = end;

                    if (state->index < state->parsedMessages.size())
                    {
                        QTimer::singleShot(0, [processChunk]() {
                            (*processChunk)();
                        });
                    }
                    else
                    {
                        // Notify user about a possible gap in logs if it returned some messages
                        // but isn't currently joined to a channel
                        const auto errorCode =
                            state->root.value("error_code").toString();
                        if (!errorCode.isEmpty())
                        {
                            qCDebug(LOG)
                                << QString("Got error from API: error_code=%1, "
                                           "channel=%2")
                                       .arg(errorCode, sharedChannel->getName());
                            if (errorCode == "channel_not_joined" &&
                                !state->builtMessages.empty())
                            {
                                sharedChannel->addSystemMessage(
                                    "Message history service recovering, there "
                                    "may "
                                    "be gaps in the message history.");
                            }
                        }

                        onLoaded(state->builtMessages);
                    }
                };

                // Start the asynchronous chunked build loop
                (*processChunk)();
            })
            .onError([channelPtr, onError](const NetworkResult &result) {
                auto shared = channelPtr.lock();
                if (!shared)
                {
                    return;
                }
                assert(!isAppAboutToQuit());

                qCDebug(LOG) << "Failed to load recent messages for"
                             << shared->getName();

                shared->addSystemMessage(
                    QStringLiteral(
                        "Message history service unavailable (Error: %1)")
                        .arg(result.formatError()));

                onError();
            })
            .execute();
    });
}

}  // namespace chatterino::recentmessages
