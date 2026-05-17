// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/TwitchChannel.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/Env.hpp"
#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "controllers/twitch/LiveController.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageThread.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/bttv/BttvLiveUpdates.hpp"
#include "providers/bttv/liveupdates/BttvLiveUpdateMessages.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/ffz/FfzBadges.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/recentmessages/Api.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvEventAPI.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/ChannelPointReward.hpp"
#include "providers/twitch/eventsub/Controller.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/TwitchUsers.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Toasts.hpp"
#include "singletons/WindowManager.hpp"
#include "util/FormatTime.hpp"
#include "util/Helpers.hpp"
#include "util/PostToThread.hpp"
#include "util/QStringHash.hpp"
#include "util/VectorMessageSink.hpp"
#include "widgets/Window.hpp"

#include <IrcConnection>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringBuilder>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QUrlQuery>
#include <rapidjson/document.h>

#include <algorithm>

namespace chatterino {

using namespace literals;
using namespace std::chrono_literals;

namespace detail {

bool isUnknownCommand(const QString &text)
{
    static QRegularExpression isUnknownCommand(
        R"(^(?:\.(?!\.|$)|\/)(?!me(?:\s|$)|\s))",
        QRegularExpression::CaseInsensitiveOption);

    auto match = isUnknownCommand.match(text);

    return match.hasMatch();
}

}  // namespace detail

using detail::isUnknownCommand;

namespace {
const QString MAGIC_MESSAGE_SUFFIX = u" \u034f"_s;
constexpr int CLIP_CREATION_COOLDOWN = 5000;
const QString CLIPS_LINK("https://clips.twitch.tv/%1");
const QString CLIPS_FAILURE_CLIPS_UNAVAILABLE_TEXT(
    "Failed to create a clip - clips are temporarily unavailable: %1");
const QString CLIPS_FAILURE_CLIPS_DISABLED_TEXT(
    "Failed to create a clip - the streamer has clips disabled in their "
    "channel.");
const QString CLIPS_FAILURE_CLIPS_RESTRICTED_TEXT(
    "Failed to create a clip - the streamer has restricted clip creation "
    "to subscribers, or followers of an unknown duration.");
const QString CLIPS_FAILURE_CLIPS_RESTRICTED_CATEGORY_TEXT(
    "Failed to create a clip - the streamer has disabled clips while in "
    "this category.");
const QString CLIPS_FAILURE_NOT_AUTHENTICATED_TEXT(
    "Failed to create a clip - you need to re-authenticate.");
const QString CLIPS_FAILURE_UNKNOWN_ERROR_TEXT("Failed to create a clip: %1");
const QString LOGIN_PROMPT_TEXT("Click here to add your account again.");
const Link ACCOUNTS_LINK(Link::OpenAccountsPage, QString());

// Maximum number of chatters to fetch when refreshing chatters
constexpr auto MAX_CHATTERS_TO_FETCH = 5000;

// From Twitch docs - expected size for a badge (1x)
constexpr QSize BASE_BADGE_SIZE(18, 18);

}  // namespace

TwitchChannel::TwitchChannel(const QString &name, bool isWatching)
    : Channel(name, Channel::Type::Twitch)
    , ChannelChatters(*static_cast<Channel *>(this))
    , nameOptions{name, name, name}
    , subscriptionUrl_("https://www.twitch.tv/subs/" + name)
    , channelUrl_("https://www.twitch.tv/" + name)
    , popoutPlayerUrl_(TWITCH_PLAYER_URL.arg(name))
    , localTwitchEmotes_(std::make_shared<EmoteMap>())
    , bttvEmotes_(std::make_shared<EmoteMap>())
    , ffzEmotes_(std::make_shared<EmoteMap>())
    , seventvEmotes_(std::make_shared<EmoteMap>())
{
    qCDebug(chatterinoTwitch) << "[TwitchChannel" << name << "] Opened";

    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->twitch.currentUserAboutToChange,
        [this](const auto & /*oldAccount*/, const auto & /*newAccount*/) {
            this->eventSubChannelChatUserMessageHoldHandle.reset();
            this->eventSubChannelChatUserMessageUpdateHandle.reset();
            this->eventSubChannelModerateHandle.reset();
            this->eventSubAutomodMessageHoldHandle.reset();
            this->eventSubAutomodMessageUpdateHandle.reset();
            this->eventSubSuspiciousUserMessageHandle.reset();
            this->eventSubSuspiciousUserUpdateHandle.reset();
        });

    this->bSignals_.emplace_back(
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            this->setMod(false);
            this->refreshPubSub();
            this->refreshTwitchChannelEmotes(false);
        }));

    this->refreshPubSub();
    // We can safely ignore this signal connection since it's a private signal, meaning
    // it will only ever be invoked by TwitchChannel itself
    std::ignore = this->userStateChanged.connect([this] {
        this->refreshPubSub();
    });

    // We can safely ignore this signal connection this has no external dependencies - once the signal
    // is destroyed, it will no longer be able to fire
    std::ignore = this->joined.connect([this]() {
        if (this->disconnected_)
        {
            this->loadRecentMessagesReconnect();
            this->lastConnectedAt_ = std::chrono::system_clock::now();
            this->disconnected_ = false;
        }
    });

    // timers
    QObject::connect(&this->chattersListTimer_, &QTimer::timeout, [this] {
        this->refreshChatters();
    });

    this->chattersListTimer_.start(5 * 60 * 1000);

    QObject::connect(&this->threadClearTimer_, &QTimer::timeout, [this] {
        // We periodically check for any dangling reply threads that missed
        // being cleaned up on messageRemovedFromStart. This could occur if
        // some other part of the program, like a user card, held a reference
        // to the message.
        //
        // It seems difficult to actually replicate a situation where things
        // are actually cleaned up, but I've verified that cleanups DO happen.
        this->cleanUpReplyThreads();
    });
    this->threadClearTimer_.start(5 * 60 * 1000);

    QObject::connect(&this->pinnedMessageTimer_, &QTimer::timeout, [this] {
        this->fetchPinnedMessage();
    });
    this->pinnedMessageTimer_.start(10 * 1000);
    // Fetch immediately on construction
    QMetaObject::invokeMethod(
        &this->lifetimeGuard_,
        [this] {
            this->fetchPinnedMessage();
        },
        Qt::QueuedConnection);

    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->twitch.emotesReloaded,
        [this](auto *caller, const auto &result) {
            if (result)
            {
                // emotes were reloaded - clear follower emotes if the user is
                // now subscribed to the streamer
                if (!this->localTwitchEmotes_.get()->empty() &&
                    getApp()->getAccounts()->twitch.getCurrent()->hasEmoteSet(
                        EmoteSetId{this->localTwitchEmoteSetID_.get()}))
                {
                    this->localTwitchEmotes_.set(std::make_shared<EmoteMap>());
                }

                if (caller == this)
                {
                    this->addSystemMessage(
                        "Twitch subscriber emotes reloaded.");
                }
                return;
            }

            if (caller == this || caller == nullptr)
            {
                this->addSystemMessage(
                    u"Failed to load Twitch subscriber emotes: " %
                    result.error());
            }
        });

    QObject::connect(&this->sendWaitTimer_, &QTimer::timeout,
                     &this->lifetimeGuard_, [this] {
                         this->syncSendWaitTimer();
                     });

    this->signalHolder_.managedConnect(
        getApp()->getTwitchPubSub()->pointReward.userBalanceUpdated,
        [this](int balance, const QString &channelId) {
            if (channelId == this->roomId())
            {
                this->channelPointsBalanceUpdated.invoke(balance);
            }
        });

    // debugging
#if 0
    for (int i = 0; i < 1000; i++) {
        this->addSystemMessage("asef");
    }
#endif
}

TwitchChannel::~TwitchChannel()
{
    if (isAppAboutToQuit())
    {
        return;
    }

    getApp()->getTwitch()->dropSeventvChannel(this->seventvUserID_,
                                              this->seventvEmoteSetID_);

    if (getApp()->getBttvLiveUpdates())
    {
        getApp()->getBttvLiveUpdates()->partChannel(this->roomId());
    }

    if (getApp()->getSeventvEventAPI())
    {
        getApp()->getSeventvEventAPI()->unsubscribeTwitchChannel(
            this->roomId());
    }

    this->destroyed.invoke();
}

void TwitchChannel::initialize()
{
    this->refreshChatters();
    this->refreshBadges();
}

void TwitchChannel::fetchPinnedMessage()
{
    if (this->getName().isEmpty())
    {
        return;
    }

    static const QString CLIENT_ID = u"kimne78kx3ncx6brgo4mv6wki5h1ko"_s;
    static const QString GQL_QUERY =
        u"query GetPins($login:String!){user(login:$login){channel{"
        u"pinnedChatMessages(first:1){edges{node{id updatedAt pinnedBy{login displayName} pinnedMessage{"
        u"id sentAt content{text}sender{id login displayName chatColor}}}}}}}}"_s;

    QJsonObject payload{
        {"operationName", "GetPins"},
        {"query", GQL_QUERY},
        {"variables", QJsonObject{{"login", this->getName()}}},
    };

    NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
        .header("Client-Id", CLIENT_ID)
        .json(payload)
        .timeout(10000)
        .onSuccess([this](const NetworkResult &result) {
            auto json = result.parseJson();
            auto edges = json["data"]
                             .toObject()["user"]
                             .toObject()["channel"]
                             .toObject()["pinnedChatMessages"]
                             .toObject()["edges"]
                             .toArray();

            if (edges.isEmpty())
            {
                if (this->pinnedMessage_.has_value())
                {
                    this->pinnedMessage_ = std::nullopt;
                    this->pinnedMessageUpdated.invoke();
                }
                return;
            }

            auto node = edges[0].toObject()["node"].toObject();
            auto pinId = node["id"].toString();
            auto pm = node["pinnedMessage"].toObject();

            auto pinner = node["pinnedBy"].toObject();

            TwitchPinnedMessage newPin{
                .pinId = pinId,
                .messageId = pm["id"].toString(),
                .text = pm["content"].toObject()["text"].toString(),
                .senderId = pm["sender"].toObject()["id"].toString(),
                .senderLogin = pm["sender"].toObject()["login"].toString(),
                .senderName =
                    pm["sender"].toObject()["displayName"].toString(),
                .chatColor = pm["sender"].toObject()["chatColor"].toString(),
                .sentAt = QDateTime::fromString(pm["sentAt"].toString(),
                                                Qt::ISODate),
                .pinnedAt = QDateTime::fromString(node["updatedAt"].toString(),
                                                  Qt::ISODate),
                .pinnerLogin = pinner["login"].toString(),
                .pinnerName = pinner["displayName"].toString(),
            };

            if (!this->pinnedMessage_.has_value() ||
                *this->pinnedMessage_ != newPin)
            {
                this->pinnedMessage_ = std::move(newPin);
                this->pinnedMessageUpdated.invoke();
            }
        })
        .onError([](const NetworkResult & /*result*/) {
            // Silently ignore - channel may not exist or API may be down
        })
        .execute();
}

void TwitchChannel::pinMessage(const QString &messageID)
{
    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        this->addSystemMessage("You must be logged in to pin a message.");
        return;
    }

    auto rid = this->roomId();
    if (rid.isEmpty())
    {
        return;
    }

    static const QString GQL_CLIENT_ID = u"kimne78kx3ncx6brgo4mv6wki5h1ko"_s;

    QString gqlToken = getSettings()->gqlAuthToken;
    if (gqlToken.isEmpty())
    {
        this->addSystemMessage(
            "No GQL auth token configured. Go to Settings > Banerino > "
            "Twitch GQL Authentication to set up your token.");
        return;
    }

    if (gqlToken.startsWith("oauth:"))
    {
        gqlToken = gqlToken.mid(6);
    }

    QJsonObject payload{
        {"operationName", "PinChatMessage"},
        {"variables", QJsonObject{
            {"input", QJsonObject{
                {"channelID", this->roomId()},
                {"messageID", messageID},
                {"type", "MOD"}
            }}
        }},
        {"extensions", QJsonObject{
            {"persistedQuery", QJsonObject{
                {"version", 1},
                {"sha256Hash", "214191369c21f1ad67ac074795d53832329c70e4088c979040c9f86334a7d736"}
            }}
        }}
    };

    NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
        .header("Client-Id", GQL_CLIENT_ID)
        .header("Authorization", "OAuth " + gqlToken)
        .json(payload)
        .onSuccess([this](const NetworkResult &) {
            QTimer::singleShot(500, [this] {
                this->fetchPinnedMessage();
            });
        })
        .onError([this](const NetworkResult &result) {
            this->addSystemMessage("Failed to pin message: " + result.formatError());
        })
        .execute();
}



void TwitchChannel::unpinMessage()
{
    if (!this->pinnedMessage_.has_value())
    {
        return;
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        this->addSystemMessage("You must be logged in to unpin a message.");
        return;
    }

    auto pinId = this->pinnedMessage_->pinId;

    static const QString GQL_CLIENT_ID = u"kimne78kx3ncx6brgo4mv6wki5h1ko"_s;

    QString gqlToken = getSettings()->gqlAuthToken;
    if (gqlToken.isEmpty())
    {
        this->addSystemMessage(
            "No GQL auth token configured. Go to Settings > Banerino > "
            "Twitch GQL Authentication to set up your token.");
        return;
    }

    if (gqlToken.startsWith("oauth:"))
    {
        gqlToken = gqlToken.mid(6);
    }

    QJsonObject payload{
        {"operationName", "unpinChatMessage"},
        {"variables", QJsonObject{
            {"input", QJsonObject{
                {"id", pinId},
                {"reason", "UNPIN"}
            }}
        }},
        {"extensions", QJsonObject{
            {"persistedQuery", QJsonObject{
                {"version", 1},
                {"sha256Hash", "86409b9c86510bdc9f2c6d8e58fdc4041963c001de53577160ab649e03334511"}
            }}
        }}
    };

    NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
        .header("Client-Id", GQL_CLIENT_ID)
        .header("Authorization", "OAuth " + gqlToken)
        .json(payload)
        .onSuccess([this](const NetworkResult &) {
            this->pinnedMessage_ = std::nullopt;
            this->pinnedMessageUpdated.invoke();
        })
        .onError([this](const NetworkResult &result) {
            this->addSystemMessage("Failed to unpin message: " + result.formatError());
        })
        .execute();
}

void TwitchChannel::createPoll(const QString &title, const QStringList &choices, int durationSeconds,
                               int pointsPerVote, std::function<void()> successCallback,
                               std::function<void(const QString &)> failureCallback)
{
    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        failureCallback("You must be logged in to create a poll.");
        return;
    }

    auto rid = this->roomId();
    if (rid.isEmpty())
    {
        failureCallback("Invalid channel ID.");
        return;
    }

    static const QString GQL_CLIENT_ID = u"kimne78kx3ncx6brgo4mv6wki5h1ko"_s;
    QString gqlToken = getSettings()->gqlAuthToken;
    if (gqlToken.isEmpty())
    {
        failureCallback("No GQL auth token configured. Go to Settings > Banerino > Twitch GQL Authentication to set up your token.");
        return;
    }

    if (gqlToken.startsWith("oauth:"))
    {
        gqlToken = gqlToken.mid(6);
    }

    QJsonArray choicesArray;
    for (const auto &choice : choices)
    {
        choicesArray.append(QJsonObject{{"title", choice}});
    }

    QJsonObject inputObj{
        {"title", title},
        {"durationSeconds", durationSeconds},
        {"ownedBy", rid},
        {"choices", choicesArray}
    };

    if (pointsPerVote > 0)
    {
        inputObj["channelPointsPerVote"] = pointsPerVote;
    }

    QJsonObject payload{
        {"operationName", "CreatePoll"},
        {"variables", QJsonObject{
            {"input", inputObj}
        }},
        {"extensions", QJsonObject{
            {"persistedQuery", QJsonObject{
                {"version", 1},
                {"sha256Hash", "4b1461a13fe166a59044961db192747d606f71a89abc3bfdecf79fe862d205cf"}
            }}
        }}
    };

    NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
        .header("Client-Id", GQL_CLIENT_ID)
        .header("Authorization", "OAuth " + gqlToken)
        .json(payload)
        .onSuccess([successCallback, failureCallback](const NetworkResult &result) {
            auto obj = result.parseJson();
            if (obj.contains("errors")) {
                auto errors = obj["errors"].toArray();
                if (!errors.isEmpty()) {
                    auto msg = errors.first().toObject()["message"].toString();
                    failureCallback(msg);
                    return;
                }
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(result.formatError());
        })
        .execute();
}

void TwitchChannel::voteInPoll(const QString &pollId, const QString &choiceId,
                               std::function<void()> successCallback,
                               std::function<void(const QString &)> failureCallback)
{
    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        failureCallback("You must be logged in to vote.");
        return;
    }

    static const QString GQL_CLIENT_ID = u"kimne78kx3ncx6brgo4mv6wki5h1ko"_s;
    QString gqlToken = getSettings()->gqlAuthToken;
    if (gqlToken.isEmpty())
    {
        failureCallback("No GQL auth token configured. Go to Settings > Banerino > Twitch GQL Authentication to set up your token.");
        return;
    }

    if (gqlToken.startsWith("oauth:"))
    {
        gqlToken = gqlToken.mid(6);
    }

    // Generate a random UUID for the voteID
    QString voteId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QJsonObject inputObj{
        {"choiceID", choiceId},
        {"pollID", pollId},
        {"userID", currentUser->getUserId()},
        {"voteID", voteId}
    };

    QJsonObject payload{
        {"operationName", "VoteInPoll"},
        {"variables", QJsonObject{
            {"input", inputObj}
        }},
        {"query", "mutation VoteInPoll($input: VoteInPollInput!) { voteInPoll(input: $input) { error { code } } }"}
    };

    NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
        .header("Client-Id", GQL_CLIENT_ID)
        .header("Authorization", "OAuth " + gqlToken)
        .json(payload)
        .onSuccess([successCallback, failureCallback](const NetworkResult &result) {
            auto obj = result.parseJson();
            if (obj.contains("errors")) {
                auto errors = obj["errors"].toArray();
                if (!errors.isEmpty()) {
                    auto msg = errors.first().toObject()["message"].toString();
                    failureCallback(msg);
                    return;
                }
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(result.formatError());
        })
        .execute();
}

void TwitchChannel::getActivePoll(std::function<void(std::optional<HelixPoll>)> successCallback,
                                  std::function<void(const QString &)> failureCallback)
{
    static const QString GQL_CLIENT_ID = u"kimne78kx3ncx6brgo4mv6wki5h1ko"_s;
    QString gqlToken = getSettings()->gqlAuthToken;
    if (gqlToken.startsWith("oauth:"))
    {
        gqlToken = gqlToken.mid(6);
    }

    QJsonObject payload{
        {"extensions", QJsonObject{
            {"persistedQuery", QJsonObject{
                {"version", 1},
                {"sha256Hash", "e83188a3836c636393df3191665e543a03733d7c51d3ade3d85e42aa46c2bf55"}
            }}
        }},
        {"variables", QJsonObject{
            {"login", this->getName()}
        }}
    };

    NetworkRequest req("https://gql.twitch.tv/gql", NetworkRequestType::Post);
    req = std::move(req).header("Client-Id", GQL_CLIENT_ID);
    req = std::move(req).json(payload);
        
    if (!gqlToken.isEmpty()) {
        req = std::move(req).header("Authorization", "OAuth " + gqlToken);
    }

    std::move(req).onSuccess([successCallback, failureCallback](const NetworkResult &result) {
            auto obj = result.parseJson();
            if (obj.contains("errors")) {
                auto errors = obj["errors"].toArray();
                if (!errors.isEmpty()) {
                    auto msg = errors.first().toObject()["message"].toString();
                    failureCallback(msg);
                    return;
                }
            }
            
            auto channelObj = obj["data"].toObject()["channel"].toObject();
            if (!channelObj.contains("viewablePoll") || channelObj["viewablePoll"].isNull()) {
                successCallback(std::nullopt);
                return;
            }
            
            auto pollObj = channelObj["viewablePoll"].toObject();
            
            QJsonObject pollObjTwitch;
            pollObjTwitch["id"] = pollObj["id"].toString();
            pollObjTwitch["title"] = pollObj["title"].toString();
            pollObjTwitch["status"] = pollObj["status"].toString();
            pollObjTwitch["duration"] = pollObj["durationSeconds"].toInt();
            pollObjTwitch["started_at"] = pollObj["startedAt"].toString();
            pollObjTwitch["ended_at"] = pollObj["endedAt"].toString();
            pollObjTwitch["channel_points_voting_enabled"] = false;
            pollObjTwitch["channel_points_per_vote"] = 0;

            QJsonArray choicesArrTwitch;
            auto choicesArray = pollObj["choices"].toArray();
            for (auto choiceVal : choicesArray) {
                auto choiceObj = choiceVal.toObject();
                QJsonObject c;
                c["id"] = choiceObj["id"].toString();
                c["title"] = choiceObj["title"].toString();
                auto votesObj = choiceObj["votes"].toObject();
                c["votes"] = votesObj.contains("total") ? votesObj["total"].toInt() : choiceObj["votes"].toInt();
                choicesArrTwitch.append(c);
            }
            pollObjTwitch["choices"] = choicesArrTwitch;
            
            HelixPoll poll(pollObjTwitch);
            
            successCallback(poll);
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(result.formatError());
        })
        .execute();
}

void TwitchChannel::terminatePoll(const QString &pollId,
                                   std::function<void()> successCallback,
                                   std::function<void(const QString &)> failureCallback)
{
    static const QString GQL_CLIENT_ID = u"kimne78kx3ncx6brgo4mv6wki5h1ko"_s;
    QString gqlToken = getSettings()->gqlAuthToken;
    if (gqlToken.isEmpty())
    {
        failureCallback("No GQL auth token configured.");
        return;
    }
    if (gqlToken.startsWith("oauth:"))
    {
        gqlToken = gqlToken.mid(6);
    }

    QJsonObject payload{
        {"extensions", QJsonObject{
            {"persistedQuery", QJsonObject{
                {"version", 1},
                {"sha256Hash", "2701ef0594dae5f532ce68e58cc3036a6d020755eef49927f98c14017fd819b2"}
            }}
        }},
        {"variables", QJsonObject{
            {"input", QJsonObject{
                {"pollID", pollId}
            }}
        }}
    };

    NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
        .header("Client-Id", GQL_CLIENT_ID)
        .header("Authorization", "OAuth " + gqlToken)
        .json(payload)
        .onSuccess([successCallback, failureCallback](const NetworkResult &result) {
            auto obj = result.parseJson();
            if (obj.contains("errors")) {
                auto errors = obj["errors"].toArray();
                if (!errors.isEmpty()) {
                    failureCallback(errors.first().toObject()["message"].toString());
                    return;
                }
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(result.formatError());
        })
        .execute();
}

void TwitchChannel::archivePoll(const QString &pollId,
                                 std::function<void()> successCallback,
                                 std::function<void(const QString &)> failureCallback)
{
    static const QString GQL_CLIENT_ID = u"kimne78kx3ncx6brgo4mv6wki5h1ko"_s;
    QString gqlToken = getSettings()->gqlAuthToken;
    if (gqlToken.isEmpty())
    {
        failureCallback("No GQL auth token configured.");
        return;
    }
    if (gqlToken.startsWith("oauth:"))
    {
        gqlToken = gqlToken.mid(6);
    }

    QJsonObject payload{
        {"extensions", QJsonObject{
            {"persistedQuery", QJsonObject{
                {"version", 1},
                {"sha256Hash", "444ead3d68d94601cb66519e36c9f6c6fd9ba8b827a4299b8ed3604e57918d92"}
            }}
        }},
        {"variables", QJsonObject{
            {"input", QJsonObject{
                {"pollID", pollId}
            }}
        }}
    };

    NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
        .header("Client-Id", GQL_CLIENT_ID)
        .header("Authorization", "OAuth " + gqlToken)
        .json(payload)
        .onSuccess([successCallback, failureCallback](const NetworkResult &result) {
            auto obj = result.parseJson();
            if (obj.contains("errors")) {
                auto errors = obj["errors"].toArray();
                if (!errors.isEmpty()) {
                    failureCallback(errors.first().toObject()["message"].toString());
                    return;
                }
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(result.formatError());
        })
        .execute();
}


const std::optional<TwitchPinnedMessage> &TwitchChannel::getPinnedMessage()
    const
{
    return this->pinnedMessage_;
}

void TwitchChannel::refreshChannelPointsBalance()
{
    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon() || this->roomId().isEmpty())
    {
        qCDebug(chatterinoTwitch) << "Channel points: skipping (anon or empty roomId)";
        return;
    }

    static const QString GQL_CLIENT_ID = u"kimne78kx3ncx6brgo4mv6wki5h1ko"_s;
    QString token = getSettings()->gqlAuthToken;
    qInfo() << "[POINTS] token =" << (token.isEmpty() ? "EMPTY" : "present") << "channel =" << this->getName();
    if (token.isEmpty())
    {
        return;
    }

    if (token.startsWith("oauth:"))
    {
        token = token.mid(6);
    }

    QJsonObject payload{
        {"operationName", "ChannelPointsContext"},
        {"query",
         "query ChannelPointsContext($channelLogin:String!){channel(name:"
         "$channelLogin){communityPointsSettings{isEnabled}self{communityPoints{balance}}}}"},
        {"variables", QJsonObject{{"channelLogin", this->getName()}}},
    };

    NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
        .header("Client-Id", GQL_CLIENT_ID)
        .header("Authorization", "OAuth " + token)
        .json(payload)

        .timeout(10000)
        .onSuccess([this](const NetworkResult &result) {
            auto json = result.parseJson();
            qInfo() << "[POINTS] response for" << this->getName() << ":" << QJsonDocument(json).toJson(QJsonDocument::Compact);
            
            auto channelObj = json["data"].toObject()["channel"].toObject();
            if (channelObj.isEmpty()) {
                qInfo() << "[POINTS] channelObj is empty";
                return;
            }
            
            bool isEnabled = channelObj["communityPointsSettings"].toObject()["isEnabled"].toBool(false);
            if (!isEnabled) {
                qInfo() << "[POINTS] not enabled for" << this->getName();
                this->channelPointsBalanceUpdated.invoke(-1);
                return;
            }

            QJsonObject selfParsed = channelObj["self"].toObject();
            if (!selfParsed.isEmpty())
            {
                QJsonObject pointsObj = selfParsed["communityPoints"].toObject();
                if (pointsObj.contains("balance"))
                {
                    int balance = static_cast<int>(pointsObj["balance"].toDouble(0));
                    qInfo() << "[POINTS] balance =" << balance << "for" << this->getName();
                    this->channelPointsBalanceUpdated.invoke(balance);
                }
            }
        })
        .onError([this](const NetworkResult &result) {
            qInfo() << "[POINTS] ERROR for" << this->getName() << "status:" << result.status().value_or(-1);
        })
        .execute();
}

bool TwitchChannel::isEmpty() const
{
    return this->getName().isEmpty();
}

bool TwitchChannel::canSendMessage() const
{
    return !this->isEmpty();
}

const QString &TwitchChannel::getDisplayName() const
{
    return this->nameOptions.displayName;
}

void TwitchChannel::setDisplayName(const QString &name)
{
    this->nameOptions.displayName = name;
}

const QString &TwitchChannel::getLocalizedName() const
{
    return this->nameOptions.localizedName;
}

void TwitchChannel::setLocalizedName(const QString &name)
{
    this->nameOptions.localizedName = name;
}

void TwitchChannel::refreshTwitchChannelEmotes(bool manualRefresh)
{
    if (getApp()->isTest())
    {
        return;
    }

    if (manualRefresh)
    {
        getApp()->getAccounts()->twitch.getCurrent()->reloadEmotes(this);
    }

    // Twitch's 'Get User Emotes' doesn't assigns a different set-ID to follower
    // emotes compared to subscriber emotes.
    QString setID = TWITCH_SUB_EMOTE_SET_PREFIX % this->roomId();
    this->localTwitchEmoteSetID_.set(setID);
    if (getApp()->getAccounts()->twitch.getCurrent()->hasEmoteSet(
            EmoteSetId{setID}))
    {
        this->localTwitchEmotes_.set(std::make_shared<EmoteMap>());
        return;
    }

    auto makeEmotes = [](const auto &emotes) {
        EmoteMap map;
        for (const auto &emote : emotes)
        {
            if (emote.type != u"follower")
            {
                continue;
            }
            map.emplace(
                EmoteName{emote.name},
                getApp()->getEmotes()->getTwitchEmotes()->getOrCreateEmote(
                    EmoteId{emote.id}, EmoteName{emote.name}));
        }
        return map;
    };

    getHelix()->getFollowedChannel(
        getApp()->getAccounts()->twitch.getCurrent()->getUserId(),
        this->roomId(), nullptr,
        [weak{this->weak_from_this()}, makeEmotes](const auto &chan) {
            auto self = std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!self || !chan)
            {
                return;
            }
            getHelix()->getChannelEmotes(
                self->roomId(),
                [weak, makeEmotes](const auto &emotes) {
                    auto self =
                        std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
                    if (!self)
                    {
                        return;
                    }

                    self->localTwitchEmotes_.set(
                        std::make_shared<EmoteMap>(makeEmotes(emotes)));
                },
                [weak] {
                    auto self = weak.lock();
                    if (!self)
                    {
                        return;
                    }
                    self->addSystemMessage("Failed to load follower emotes.");
                });
        },
        [](const auto &error) {
            qCWarning(chatterinoTwitch)
                << "Failed to get following status:" << error;
        });
}

void TwitchChannel::refreshBTTVChannelEmotes(bool manualRefresh)
{
    if (!Settings::instance().enableBTTVChannelEmotes)
    {
        this->bttvEmotes_.set(EMPTY_EMOTE_MAP);
        return;
    }

    bool cacheHit = readProviderEmotesCache(
        this->roomId(), "betterttv",
        [this, weak = weakOf<Channel>(this)](auto jsonDoc) {
            if (auto shared = weak.lock())
            {
                auto emoteMap = bttv::detail::parseChannelEmotes(
                    jsonDoc.object(), this->getLocalizedName());
                this->setBttvEmotes(std::make_shared<const EmoteMap>(emoteMap));
            }
        });

    BttvEmotes::loadChannel(
        weakOf<Channel>(this), this->roomId(), this->getLocalizedName(),
        [this, weak = weakOf<Channel>(this)](auto &&emoteMap) {
            if (auto shared = weak.lock())
            {
                this->setBttvEmotes(std::make_shared<const EmoteMap>(emoteMap));
            }
        },
        manualRefresh, cacheHit);
}

void TwitchChannel::refreshFFZChannelEmotes(bool manualRefresh)
{
    if (!Settings::instance().enableFFZChannelEmotes)
    {
        this->ffzEmotes_.set(EMPTY_EMOTE_MAP);
        return;
    }

    bool cacheHit = readProviderEmotesCache(
        this->roomId(), "frankerfacez", [this](const auto &jsonDoc) {
            auto emoteMap = ffz::detail::parseChannelEmotes(jsonDoc.object());
            this->setFfzEmotes(std::make_shared<const EmoteMap>(emoteMap));
        });

    FfzEmotes::loadChannel(
        weakOf<Channel>(this), this->roomId(),
        [this, weak = weakOf<Channel>(this)](auto &&emoteMap) {
            if (auto shared = weak.lock())
            {
                this->setFfzEmotes(std::make_shared<const EmoteMap>(emoteMap));
            }
        },
        [this, weak = weakOf<Channel>(this)](auto &&modBadge) {
            if (auto shared = weak.lock())
            {
                this->ffzCustomModBadge_.set(
                    std::forward<decltype(modBadge)>(modBadge));
            }
        },
        [this, weak = weakOf<Channel>(this)](auto &&vipBadge) {
            if (auto shared = weak.lock())
            {
                this->ffzCustomVipBadge_.set(
                    std::forward<decltype(vipBadge)>(vipBadge));
            }
        },
        [this, weak = weakOf<Channel>(this)](auto &&channelBadges) {
            if (auto shared = weak.lock())
            {
                this->tgFfzChannelBadges_.guard();
                this->ffzChannelBadges_ =
                    std::forward<decltype(channelBadges)>(channelBadges);
            }
        },
        manualRefresh, cacheHit);
}

void TwitchChannel::refreshSevenTVChannelEmotes(bool manualRefresh)
{
    if (!Settings::instance().enableSevenTVChannelEmotes)
    {
        this->seventvEmotes_.set(EMPTY_EMOTE_MAP);
        return;
    }

    bool cacheHit = readProviderEmotesCache(
        this->roomId(), "seventv", [this](auto jsonDoc) {
            const auto json = jsonDoc.object();
            const auto emoteSet = json["emote_set"].toObject();
            const auto parsedEmotes = emoteSet["emotes"].toArray();
            auto emoteMap = seventv::detail::parseEmotes(
                parsedEmotes, SeventvEmoteSetKind::Channel);
            this->setSeventvEmotes(std::make_shared<const EmoteMap>(emoteMap));
        });

    SeventvEmotes::loadChannelEmotes(
        weakOf<Channel>(this), this->roomId(),
        [this, weak = weakOf<Channel>(this)](auto &&emoteMap,
                                             auto channelInfo) {
            if (auto shared = weak.lock())
            {
                this->setSeventvEmotes(
                    std::make_shared<const EmoteMap>(emoteMap));
                this->updateSeventvData(channelInfo.userID,
                                        channelInfo.emoteSetID);
                this->seventvUserTwitchConnectionIndex_ =
                    channelInfo.twitchConnectionIndex;
            }
        },
        manualRefresh, cacheHit);
}

void TwitchChannel::setBttvEmotes(std::shared_ptr<const EmoteMap> &&map)
{
    this->bttvEmotes_.set(std::move(map));
}

void TwitchChannel::setFfzEmotes(std::shared_ptr<const EmoteMap> &&map)
{
    this->ffzEmotes_.set(std::move(map));
}

void TwitchChannel::setSeventvEmotes(std::shared_ptr<const EmoteMap> &&map)
{
    this->seventvEmotes_.set(std::move(map));
}

void TwitchChannel::addQueuedRedemption(const QString &rewardId,
                                        const QString &originalContent,
                                        Communi::IrcMessage *message)
{
    this->waitingRedemptions_.push_back({
        rewardId,
        originalContent,
        {message->clone(), {}},
    });
}

void TwitchChannel::addChannelPointReward(const ChannelPointReward &reward)
{
    assertInGuiThread();

    if (!reward.isUserInputRequired)
    {
        this->addMessage(MessageBuilder::makeChannelPointRewardMessage(
                             reward, this->isMod(), this->isBroadcaster()),
                         MessageContext::Original);
        return;
    }

    bool result = false;
    {
        auto channelPointRewards = this->channelPointRewards_.access();
        result = channelPointRewards->try_emplace(reward.id, reward).second;
    }
    if (result)
    {
        const auto &channelName = this->getName();
        qCDebug(chatterinoTwitch)
            << "[TwitchChannel" << channelName
            << "] Channel point reward added:" << reward.id << ","
            << reward.title << "," << reward.isUserInputRequired;

        auto *server = getApp()->getTwitch();
        auto it = std::remove_if(
            this->waitingRedemptions_.begin(), this->waitingRedemptions_.end(),
            [&](const QueuedRedemption &msg) {
                if (reward.id == msg.rewardID)
                {
                    VectorMessageSink sink(
                        MessageSinkTrait::AddMentionsToGlobalChannel);
                    IrcMessageHandler::instance().addMessage(
                        msg.message.get(), sink, this, msg.originalContent,
                        *server, false, false);
                    if (sink.messages().empty())
                    {
                        return true;
                    }
                    MessagePtr next = sink.messages().back();
                    auto prev = this->findMessageByID(next->id);
                    if (!prev)
                    {
                        // message gone
                        this->addMessage(next, MessageContext::Repost);
                        return true;
                    }
                    this->replaceMessage(prev, next);
                    return true;
                }
                return false;
            });
        this->waitingRedemptions_.erase(it, this->waitingRedemptions_.end());
    }
}

void TwitchChannel::addKnownChannelPointReward(const ChannelPointReward &reward)
{
    assert(getApp()->isTest());

    auto channelPointRewards = this->channelPointRewards_.access();
    channelPointRewards->try_emplace(reward.id, reward);
}

bool TwitchChannel::isChannelPointRewardKnown(const QString &rewardId)
{
    const auto &pointRewards = this->channelPointRewards_.accessConst();
    const auto &it = pointRewards->find(rewardId);
    return it != pointRewards->end();
}

std::optional<ChannelPointReward> TwitchChannel::channelPointReward(
    const QString &rewardId) const
{
    auto rewards = this->channelPointRewards_.accessConst();
    auto it = rewards->find(rewardId);

    if (it == rewards->end())
    {
        return std::nullopt;
    }
    return it->second;
}

void TwitchChannel::updateStreamStatus(
    const std::optional<HelixStream> &helixStream, bool isInitialUpdate)
{
    if (helixStream)
    {
        auto stream = *helixStream;
        {
            auto status = this->streamStatus_.access();
            status->streamId = stream.id;
            status->viewerCount = stream.viewerCount;
            status->gameId = stream.gameId;
            status->game = stream.gameName;
            status->title = stream.title;
            QDateTime since =
                QDateTime::fromString(stream.startedAt, Qt::ISODate);
            auto diff = since.secsTo(QDateTime::currentDateTime());
            status->uptime = QString::number(diff / 3600) + "h " +
                             QString::number(diff % 3600 / 60) + "m";
            status->uptimeSeconds = diff;

            status->rerun = false;
            status->streamType = stream.type;
            for (const auto &tag : stream.tags)
            {
                if (QString::compare(tag, "Rerun", Qt::CaseInsensitive) == 0)
                {
                    status->rerun = true;
                    status->streamType = "rerun";
                    break;
                }
            }
        }
        if (this->setLive(true))
        {
            this->onLiveStatusChanged(true, isInitialUpdate);
        }
        this->streamStatusChanged.invoke();
    }
    else
    {
        if (this->setLive(false))
        {
            this->onLiveStatusChanged(false, isInitialUpdate);
            this->streamStatusChanged.invoke();
        }
    }
}

void TwitchChannel::onLiveStatusChanged(bool isLive, bool isInitialUpdate)
{
    // Similar code exists in NotificationController::updateFakeChannel.
    // Since we're a TwitchChannel, we also send a message here.
    if (isLive)
    {
        qCDebug(chatterinoTwitch).nospace().noquote()
            << "[TwitchChannel " << this->getName() << "] Online";

        getApp()->getNotifications()->notifyTwitchChannelLive({
            .channelId = this->roomId(),
            .channelName = this->getName(),
            .displayName = this->getDisplayName(),
            .title = this->accessStreamStatus()->title,
            .isInitialUpdate = isInitialUpdate,
        });

        // Channel live message
        this->addMessage(
            MessageBuilder::makeLiveMessage(
                this->getDisplayName(), this->roomId(),
                this->accessStreamStatus()->title,
                {MessageFlag::System, MessageFlag::DoNotTriggerNotification}),
            MessageContext::Original);
    }
    else
    {
        qCDebug(chatterinoTwitch).nospace().noquote()
            << "[TwitchChannel " << this->getName() << "] Offline";

        // Channel offline message
        this->addMessage(MessageBuilder::makeOfflineSystemMessage(
                             this->getDisplayName(), this->roomId()),
                         MessageContext::Original);

        getApp()->getNotifications()->notifyTwitchChannelOffline(
            this->roomId());
    }
};

void TwitchChannel::updateStreamTitle(const QString &title)
{
    {
        auto status = this->streamStatus_.access();
        if (status->title == title)
        {
            // Title has not changed
            return;
        }
        status->title = title;
    }
    this->streamStatusChanged.invoke();
}

void TwitchChannel::updateDisplayName(const QString &displayName)
{
    if (displayName == this->nameOptions.actualDisplayName)
    {
        // Display name has not changed
        return;
    }

    // Display name has changed

    this->nameOptions.actualDisplayName = displayName;

    if (QString::compare(displayName, this->getName(), Qt::CaseInsensitive) ==
        0)
    {
        // Display name is only a case variation of the login name
        this->setDisplayName(displayName);

        this->setLocalizedName(displayName);
    }
    else
    {
        // Display name contains Chinese, Japanese, or Korean characters
        this->setDisplayName(this->getName());

        this->setLocalizedName(
            QString("%1(%2)").arg(this->getName()).arg(displayName));
    }

    this->addRecentChatter(this->getDisplayName());

    this->displayNameChanged.invoke();
}

void TwitchChannel::showLoginMessage()
{
    const auto linkColor = MessageColor(MessageColor::Link);
    const auto accountsLink = Link(Link::OpenAccountsPage, QString());
    const auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    const auto expirationText =
        QStringLiteral("You need to log in to send messages. You can link your "
                       "Twitch account");
    const auto loginPromptText = QStringLiteral("in the settings.");

    auto builder = MessageBuilder();
    builder.message().flags.set(MessageFlag::System);
    builder.message().flags.set(MessageFlag::DoNotTriggerNotification);

    builder.emplace<TimestampElement>();
    builder.emplace<TextElement>(expirationText, MessageElementFlag::Text,
                                 MessageColor::System);
    builder
        .emplace<TextElement>(loginPromptText, MessageElementFlag::Text,
                              linkColor)
        ->setLink(accountsLink);

    this->addMessage(builder.release(), MessageContext::Original);
}

void TwitchChannel::roomIdChanged()
{
    if (getApp()->isTest())
    {
        return;
    }
    this->refreshPubSub();
    this->refreshBadges();
    this->refreshCheerEmotes();
    this->refreshTwitchChannelEmotes(false);
    this->refreshFFZChannelEmotes(false);
    this->refreshBTTVChannelEmotes(false);
    this->refreshSevenTVChannelEmotes(false);
    this->joinBttvChannel();
    this->listenSevenTVCosmetics();
    getApp()->getTwitchLiveController()->add(
        std::dynamic_pointer_cast<TwitchChannel>(shared_from_this()));
}

QString TwitchChannel::prepareMessage(const QString &message) const
{
    auto *app = getApp();
    QString parsedMessage =
        app->getEmotes()->getEmojis()->replaceShortCodes(message);

    parsedMessage = parsedMessage.simplified();

    if (parsedMessage.isEmpty())
    {
        return "";
    }

    if (!this->hasHighRateLimit())
    {
        if (getSettings()->allowDuplicateMessages)
        {
            if (parsedMessage == this->lastSentMessage_)
            {
                auto spaceIndex = parsedMessage.indexOf(' ');
                // If the message starts with either '/' or '.' Twitch will treat it as a command, omitting
                // first space and only rest of the arguments treated as actual message content
                // In cases when user sends a message like ". .a b" first character and first space are omitted as well
                bool ignoreFirstSpace =
                    parsedMessage.at(0) == '/' || parsedMessage.at(0) == '.';
                if (ignoreFirstSpace)
                {
                    spaceIndex = parsedMessage.indexOf(' ', spaceIndex + 1);
                }

                if (spaceIndex == -1)
                {
                    // no spaces found, fall back to old magic character
                    parsedMessage.append(MAGIC_MESSAGE_SUFFIX);
                }
                else
                {
                    // replace the space we found in spaceIndex with two spaces
                    parsedMessage.replace(spaceIndex, 1, "  ");
                }
            }
        }
    }

    return parsedMessage;
}

void TwitchChannel::sendMessage(const QString &message)
{
    auto *app = getApp();
    if (!app->getAccounts()->twitch.isLoggedIn())
    {
        if (!message.isEmpty())
        {
            this->showLoginMessage();
        }

        return;
    }

    qCDebug(chatterinoTwitch)
        << "[TwitchChannel" << this->getName() << "] Send message:" << message;

    // Do last message processing
    QString parsedMessage = this->prepareMessage(message);
    if (parsedMessage.isEmpty())
    {
        return;
    }

    if (getSettings()->shouldSendHelixChat() && isUnknownCommand(parsedMessage))
    {
        this->addSystemMessage(QString("%1 is not a known command.")
                                   .arg(parsedMessage.split(' ').first()));
        return;
    }

    bool messageSent = false;
    this->sendMessageSignal.invoke(parsedMessage, messageSent);
    this->updateBttvActivity();
    this->updateSevenTVActivity();

    if (messageSent)
    {
        qCDebug(chatterinoTwitch) << "sent";
        this->lastSentMessage_ = parsedMessage;
    }
}

void TwitchChannel::sendReply(const QString &message, const QString &replyId)
{
    auto *app = getApp();
    if (!app->getAccounts()->twitch.isLoggedIn())
    {
        if (!message.isEmpty())
        {
            this->showLoginMessage();
        }

        return;
    }

    qCDebug(chatterinoTwitch) << "[TwitchChannel" << this->getName()
                              << "] Send reply message:" << message;

    // Do last message processing
    QString parsedMessage = this->prepareMessage(message);
    if (parsedMessage.isEmpty())
    {
        return;
    }

    if (getSettings()->shouldSendHelixChat() && isUnknownCommand(parsedMessage))
    {
        this->addSystemMessage(QString("%1 is not a known command.")
                                   .arg(parsedMessage.split(' ').first()));
        return;
    }

    bool messageSent = false;
    this->sendReplySignal.invoke(parsedMessage, replyId, messageSent);

    if (messageSent)
    {
        qCDebug(chatterinoTwitch) << "sent";
        this->lastSentMessage_ = parsedMessage;
    }
}

bool TwitchChannel::isMod() const
{
    return this->mod_;
}

bool TwitchChannel::isVip() const
{
    return this->vip_;
}

bool TwitchChannel::isStaff() const
{
    return this->staff_;
}

void TwitchChannel::setMod(bool value)
{
    if (this->mod_ != value)
    {
        this->mod_ = value;

        this->userStateChanged.invoke();
    }
}

void TwitchChannel::setVIP(bool value)
{
    if (this->vip_ != value)
    {
        this->vip_ = value;

        this->userStateChanged.invoke();
    }
}

void TwitchChannel::setStaff(bool value)
{
    if (this->staff_ != value)
    {
        this->staff_ = value;

        this->userStateChanged.invoke();
    }
}

bool TwitchChannel::isBroadcaster() const
{
    auto *app = getApp();

    return this->getName() ==
           app->getAccounts()->twitch.getCurrent()->getUserName();
}

bool TwitchChannel::hasHighRateLimit() const
{
    return this->isMod() || this->isBroadcaster() || this->isVip();
}

bool TwitchChannel::canReconnect() const
{
    return true;
}

void TwitchChannel::reconnect()
{
    getApp()->getTwitch()->connect();
}

QString TwitchChannel::getCurrentStreamID() const
{
    auto streamStatus = this->accessStreamStatus();
    if (streamStatus->live)
    {
        return streamStatus->streamId;
    }

    return {};
}

QString TwitchChannel::roomId() const
{
    return *this->roomID_.access();
}

void TwitchChannel::setRoomId(const QString &id)
{
    if (*this->roomID_.accessConst() != id)
    {
        *this->roomID_.access() = id;
        // This is intended for tests and benchmarks. See comment in constructor.
        if (!getApp()->isTest())
        {
            this->roomIdChanged();
            this->loadRecentMessages();
        }
        this->disconnected_ = false;
        this->lastConnectedAt_ = std::chrono::system_clock::now();
    }
}

SharedAccessGuard<const TwitchChannel::RoomModes>
    TwitchChannel::accessRoomModes() const
{
    return this->roomModes.accessConst();
}

void TwitchChannel::setRoomModes(const RoomModes &newRoomModes)
{
    this->roomModes = newRoomModes;

    // Clear send wait timer when slow mode is disabled
    if (newRoomModes.slowMode == 0)
    {
        this->setSendWait(newRoomModes.slowMode);
    }

    this->roomModesChanged.invoke();
}

bool TwitchChannel::isLive() const
{
    return this->streamStatus_.accessConst()->live;
}

bool TwitchChannel::isRerun() const
{
    return this->streamStatus_.accessConst()->rerun;
}

SharedAccessGuard<const TwitchChannel::StreamStatus>
    TwitchChannel::accessStreamStatus() const
{
    return this->streamStatus_.accessConst();
}

std::optional<EmotePtr> TwitchChannel::twitchEmote(const EmoteName &name) const
{
    auto emotes = this->localTwitchEmotes();
    auto it = emotes->find(name);

    if (it == emotes->end())
    {
        return getApp()->getAccounts()->twitch.getCurrent()->twitchEmote(name);
    }
    return it->second;
}

std::optional<EmotePtr> TwitchChannel::bttvEmote(const EmoteName &name) const
{
    auto emotes = this->bttvEmotes_.get();
    auto it = emotes->find(name);

    if (it == emotes->end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::optional<EmotePtr> TwitchChannel::ffzEmote(const EmoteName &name) const
{
    auto emotes = this->ffzEmotes_.get();
    auto it = emotes->find(name);

    if (it == emotes->end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::optional<EmotePtr> TwitchChannel::seventvEmote(const EmoteName &name) const
{
    auto emotes = this->seventvEmotes_.get();
    auto it = emotes->find(name);

    if (it == emotes->end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::shared_ptr<const EmoteMap> TwitchChannel::localTwitchEmotes() const
{
    return this->localTwitchEmotes_.get();
}

std::shared_ptr<const EmoteMap> TwitchChannel::bttvEmotes() const
{
    return this->bttvEmotes_.get();
}

std::shared_ptr<const EmoteMap> TwitchChannel::ffzEmotes() const
{
    return this->ffzEmotes_.get();
}

std::shared_ptr<const EmoteMap> TwitchChannel::seventvEmotes() const
{
    return this->seventvEmotes_.get();
}

const QString &TwitchChannel::seventvUserID() const
{
    return this->seventvUserID_;
}
const QString &TwitchChannel::seventvEmoteSetID() const
{
    return this->seventvEmoteSetID_;
}

void TwitchChannel::joinBttvChannel() const
{
    if (getApp()->getBttvLiveUpdates())
    {
        const auto currentAccount =
            getApp()->getAccounts()->twitch.getCurrent();
        QString userID;
        if (currentAccount && !currentAccount->isAnon())
        {
            userID = currentAccount->getUserId();
        }
        getApp()->getBttvLiveUpdates()->joinChannel(this->roomId(), userID);
    }
}

void TwitchChannel::addBttvEmote(
    const BttvLiveUpdateEmoteUpdateAddMessage &message)
{
    auto emote = BttvEmotes::addEmote(this->getDisplayName(), this->bttvEmotes_,
                                      message);

    this->addOrReplaceLiveUpdatesAddRemove(true, "BTTV", QString() /*actor*/,
                                           emote->name.string);
}

void TwitchChannel::updateBttvEmote(
    const BttvLiveUpdateEmoteUpdateAddMessage &message)
{
    auto updated = BttvEmotes::updateEmote(this->getDisplayName(),
                                           this->bttvEmotes_, message);
    if (!updated)
    {
        return;
    }

    const auto [oldEmote, newEmote] = *updated;
    if (oldEmote->name == newEmote->name)
    {
        return;  // only the creator changed
    }

    auto builder = MessageBuilder(liveUpdatesUpdateEmoteMessage, "BTTV",
                                  QString() /* actor */, newEmote->name.string,
                                  oldEmote->name.string);
    this->addMessage(builder.release(), MessageContext::Original);
}

void TwitchChannel::removeBttvEmote(
    const BttvLiveUpdateEmoteRemoveMessage &message)
{
    auto removed = BttvEmotes::removeEmote(this->bttvEmotes_, message);
    if (!removed)
    {
        return;
    }

    this->addOrReplaceLiveUpdatesAddRemove(false, "BTTV", QString() /*actor*/,
                                           (*removed)->name.string);
}

void TwitchChannel::addSeventvEmote(
    const seventv::eventapi::EmoteAddDispatch &dispatch)
{
    if (!SeventvEmotes::addEmote(this->seventvEmotes_, dispatch))
    {
        return;
    }

    this->addOrReplaceLiveUpdatesAddRemove(
        true, "7TV", dispatch.actorName, dispatch.emoteJson["name"].toString());
}

void TwitchChannel::updateSeventvEmote(
    const seventv::eventapi::EmoteUpdateDispatch &dispatch)
{
    if (!SeventvEmotes::updateEmote(this->seventvEmotes_, dispatch))
    {
        return;
    }

    auto builder =
        MessageBuilder(liveUpdatesUpdateEmoteMessage, "7TV", dispatch.actorName,
                       dispatch.emoteName, dispatch.oldEmoteName);
    this->addMessage(builder.release(), MessageContext::Original);
}

void TwitchChannel::removeSeventvEmote(
    const seventv::eventapi::EmoteRemoveDispatch &dispatch)
{
    auto removed = SeventvEmotes::removeEmote(this->seventvEmotes_, dispatch);
    if (!removed)
    {
        return;
    }

    this->addOrReplaceLiveUpdatesAddRemove(false, "7TV", dispatch.actorName,
                                           (*removed)->name.string);
}

void TwitchChannel::updateSeventvUser(
    const seventv::eventapi::UserConnectionUpdateDispatch &dispatch)
{
    if (dispatch.connectionIndex != this->seventvUserTwitchConnectionIndex_)
    {
        // A different connection was updated
        return;
    }

    this->updateSeventvData(this->seventvUserID_, dispatch.emoteSetID);
    SeventvEmotes::getEmoteSet(
        dispatch.emoteSetID,
        [this, weak = weakOf<Channel>(this), dispatch](auto &&emotes,
                                                       const auto &name) {
            postToThread([this, weak, dispatch, emotes, name]() {
                if (auto shared = weak.lock())
                {
                    this->seventvEmotes_.set(
                        std::make_shared<EmoteMap>(emotes));
                    auto builder =
                        MessageBuilder(liveUpdatesUpdateEmoteSetMessage, "7TV",
                                       dispatch.actorName, name);
                    this->addMessage(builder.release(),
                                     MessageContext::Original);
                }
            });
        },
        [this, weak = weakOf<Channel>(this)](const auto &reason) {
            postToThread([this, weak, reason]() {
                if (auto shared = weak.lock())
                {
                    this->seventvEmotes_.set(EMPTY_EMOTE_MAP);
                    this->addSystemMessage(
                        QString("Failed updating 7TV emote set (%1).")
                            .arg(reason));
                }
            });
        });
}

void TwitchChannel::updateSeventvData(const QString &newUserID,
                                      const QString &newEmoteSetID)
{
    if (this->seventvUserID_ == newUserID &&
        this->seventvEmoteSetID_ == newEmoteSetID)
    {
        return;
    }

    const auto oldUserID = makeConditionedOptional(
        !this->seventvUserID_.isEmpty() && this->seventvUserID_ != newUserID,
        this->seventvUserID_);
    const auto oldEmoteSetID =
        makeConditionedOptional(!this->seventvEmoteSetID_.isEmpty() &&
                                    this->seventvEmoteSetID_ != newEmoteSetID,
                                this->seventvEmoteSetID_);

    this->seventvUserID_ = newUserID;
    this->seventvEmoteSetID_ = newEmoteSetID;
    runInGuiThread([this, oldUserID, oldEmoteSetID]() {
        if (getApp()->getSeventvEventAPI())
        {
            getApp()->getSeventvEventAPI()->subscribeUser(
                this->seventvUserID_, this->seventvEmoteSetID_);

            if (oldUserID || oldEmoteSetID)
            {
                getApp()->getTwitch()->dropSeventvChannel(
                    oldUserID.value_or(QString()),
                    oldEmoteSetID.value_or(QString()));
            }
        }
    });
}

void TwitchChannel::addOrReplaceLiveUpdatesAddRemove(bool isEmoteAdd,
                                                     const QString &platform,
                                                     const QString &actor,
                                                     const QString &emoteName)
{
    if (this->tryReplaceLastLiveUpdateAddOrRemove(
            isEmoteAdd ? MessageFlag::LiveUpdatesAdd
                       : MessageFlag::LiveUpdatesRemove,
            platform, actor, emoteName))
    {
        return;
    }

    this->lastLiveUpdateEmoteNames_ = {emoteName};

    MessagePtr msg;
    if (isEmoteAdd)
    {
        msg = MessageBuilder(liveUpdatesAddEmoteMessage, platform, actor,
                             this->lastLiveUpdateEmoteNames_)
                  .release();
    }
    else
    {
        msg = MessageBuilder(liveUpdatesRemoveEmoteMessage, platform, actor,
                             this->lastLiveUpdateEmoteNames_)
                  .release();
    }
    this->lastLiveUpdateEmotePlatform_ = platform;
    this->lastLiveUpdateMessage_ = msg;
    this->lastLiveUpdateEmoteActor_ = actor;
    this->addMessage(msg, MessageContext::Original);
}

bool TwitchChannel::tryReplaceLastLiveUpdateAddOrRemove(
    MessageFlag op, const QString &platform, const QString &actor,
    const QString &emoteName)
{
    if (this->lastLiveUpdateEmotePlatform_ != platform)
    {
        return false;
    }
    auto last = this->lastLiveUpdateMessage_.lock();
    if (!last || !last->flags.has(op) ||
        last->parseTime < QTime::currentTime().addSecs(-5) ||
        last->loginName != actor)
    {
        return false;
    }
    // Update the message
    this->lastLiveUpdateEmoteNames_.push_back(emoteName);

    auto makeReplacement = [&](MessageFlag op) -> MessageBuilder {
        if (op == MessageFlag::LiveUpdatesAdd)
        {
            return {
                liveUpdatesAddEmoteMessage,
                platform,
                last->loginName,
                this->lastLiveUpdateEmoteNames_,
            };
        }

        // op == RemoveEmoteMessage
        return {
            liveUpdatesRemoveEmoteMessage,
            platform,
            last->loginName,
            this->lastLiveUpdateEmoteNames_,
        };
    };

    auto replacement = makeReplacement(op);

    replacement->flags = last->flags;

    auto msg = replacement.release();
    this->lastLiveUpdateMessage_ = msg;
    this->replaceMessage(last, msg);

    return true;
}

void TwitchChannel::messageRemovedFromStart(const MessagePtr &msg)
{
    if (msg->replyThread)
    {
        if (msg->replyThread->liveCount(msg) == 0)
        {
            this->threads_.erase(msg->replyThread->rootId());
        }
    }
}

const QString &TwitchChannel::subscriptionUrl()
{
    return this->subscriptionUrl_;
}

const QString &TwitchChannel::channelUrl()
{
    return this->channelUrl_;
}

const QString &TwitchChannel::popoutPlayerUrl()
{
    return this->popoutPlayerUrl_;
}

int TwitchChannel::chatterCount() const
{
    return this->chatterCount_;
}

bool TwitchChannel::setLive(bool newLiveStatus)
{
    auto guard = this->streamStatus_.access();
    if (guard->live == newLiveStatus)
    {
        return false;
    }
    guard->live = newLiveStatus;
    if (!newLiveStatus)
    {
        // A rerun is just a fancy livestream
        guard->rerun = false;
    }

    return true;
}

void TwitchChannel::markConnected()
{
    if (this->lastConnectedAt_.has_value() && !this->disconnected_)
    {
        this->lastConnectedAt_ = std::chrono::system_clock::now();
    }
}

void TwitchChannel::markDisconnected()
{
    if (this->roomId().isEmpty())
    {
        // we were never joined in the first place
        return;
    }

    this->disconnected_ = true;
}

void TwitchChannel::loadRecentMessages()
{
    if (!getSettings()->loadTwitchMessageHistoryOnConnect)
    {
        return;
    }

    if (this->loadingRecentMessages_.test_and_set())
    {
        return;  // already loading
    }

    auto weak = weakOf<Channel>(this);
    recentmessages::load(
        this->getName(), weak,
        [weak](const auto &messages) {
            assert(!isAppAboutToQuit());
            auto shared = weak.lock();
            if (!shared)
            {
                return;
            }

            auto *tc = dynamic_cast<TwitchChannel *>(shared.get());
            if (!tc)
            {
                return;
            }

            tc->addMessagesAtStart(messages);
            tc->loadingRecentMessages_.clear();

            std::vector<MessagePtr> msgs;
            for (const auto &msg : messages)
            {
                const auto highlighted =
                    msg->flags.has(MessageFlag::Highlighted);
                const auto showInMentions =
                    msg->flags.has(MessageFlag::ShowInMentions);
                if (highlighted && showInMentions)
                {
                    msgs.push_back(msg);
                }
            }

            getApp()->getTwitch()->getMentionsChannel()->fillInMissingMessages(
                msgs);
        },
        [weak]() {
            auto shared = weak.lock();
            if (!shared)
            {
                return;
            }

            auto *tc = dynamic_cast<TwitchChannel *>(shared.get());
            if (!tc)
            {
                return;
            }

            tc->loadingRecentMessages_.clear();
        },
        getSettings()->twitchMessageHistoryLimit.getValue(), std::nullopt,
        std::nullopt, false);
}

void TwitchChannel::loadRecentMessagesReconnect()
{
    if (!getSettings()->loadTwitchMessageHistoryOnConnect)
    {
        return;
    }

    if (this->loadingRecentMessages_.test_and_set())
    {
        return;  // already loading
    }

    const auto now = std::chrono::system_clock::now();
    int limit = getSettings()->twitchMessageHistoryLimit.getValue();
    if (this->lastConnectedAt_.has_value())
    {
        // calculate how many messages could have occurred
        // while we were not connected to the channel
        // assuming a maximum of 10 messages per second
        const auto secondsSinceDisconnect =
            std::chrono::duration_cast<std::chrono::seconds>(
                now - this->lastConnectedAt_.value())
                .count();
        limit =
            std::min(static_cast<int>(secondsSinceDisconnect + 1) * 10, limit);
    }

    auto weak = weakOf<Channel>(this);
    recentmessages::load(
        this->getName(), weak,
        [weak](const auto &messages) {
            auto shared = weak.lock();
            if (!shared)
            {
                return;
            }

            auto *tc = dynamic_cast<TwitchChannel *>(shared.get());
            if (!tc)
            {
                return;
            }

            tc->fillInMissingMessages(messages);
            tc->loadingRecentMessages_.clear();
        },
        [weak]() {
            auto shared = weak.lock();
            if (!shared)
            {
                return;
            }

            auto *tc = dynamic_cast<TwitchChannel *>(shared.get());
            if (!tc)
            {
                return;
            }

            tc->loadingRecentMessages_.clear();
        },
        limit, this->lastConnectedAt_, now, true);
}

void TwitchChannel::refreshPubSub()
{
    if (getApp()->isTest())
    {
        return;
    }

    auto roomId = this->roomId();
    if (roomId.isEmpty())
    {
        return;
    }

    auto currentAccount = getApp()->getAccounts()->twitch.getCurrent();

    getApp()->getTwitchPubSub()->listenToChannelPointRewards(roomId);

    if (!currentAccount->isAnon())
    {
        getApp()->getTwitchPubSub()->listenToUserPointBalance(currentAccount->getUserId());
        this->refreshChannelPointsBalance();
    }

    if (currentAccount->isAnon())
    {
        this->eventSubChannelModerateHandle.reset();
        this->eventSubAutomodMessageHoldHandle.reset();
        this->eventSubAutomodMessageUpdateHandle.reset();
        this->eventSubSuspiciousUserMessageHandle.reset();
        this->eventSubSuspiciousUserUpdateHandle.reset();
        this->eventSubChannelChatUserMessageHoldHandle.reset();
        this->eventSubChannelChatUserMessageUpdateHandle.reset();
        return;
    }

    const auto &currentTwitchUserID = currentAccount->getUserId();

    if (this->hasModRights())
    {
        this->eventSubChannelModerateHandle =
            getApp()->getEventSub()->subscribe(eventsub::SubscriptionRequest{
                .subscriptionType = "channel.moderate",
                .subscriptionVersion = "2",
                .ownerTwitchUserID = currentTwitchUserID,
                .conditions =
                    {
                        {
                            "broadcaster_user_id",
                            roomId,
                        },
                        {
                            "moderator_user_id",
                            currentTwitchUserID,
                        },
                    },
            });
        this->eventSubAutomodMessageHoldHandle =
            getApp()->getEventSub()->subscribe(eventsub::SubscriptionRequest{
                .subscriptionType = "automod.message.hold",
                .subscriptionVersion = "2",
                .ownerTwitchUserID = currentTwitchUserID,
                .conditions =
                    {
                        {
                            "broadcaster_user_id",
                            roomId,
                        },
                        {
                            "moderator_user_id",
                            currentTwitchUserID,
                        },
                    },
            });
        this->eventSubAutomodMessageUpdateHandle =
            getApp()->getEventSub()->subscribe(eventsub::SubscriptionRequest{
                .subscriptionType = "automod.message.update",
                .subscriptionVersion = "2",
                .ownerTwitchUserID = currentTwitchUserID,
                .conditions =
                    {
                        {
                            "broadcaster_user_id",
                            roomId,
                        },
                        {
                            "moderator_user_id",
                            currentTwitchUserID,
                        },
                    },
            });
        this->eventSubSuspiciousUserMessageHandle =
            getApp()->getEventSub()->subscribe(eventsub::SubscriptionRequest{
                .subscriptionType = "channel.suspicious_user.message",
                .subscriptionVersion = "1",
                .ownerTwitchUserID = currentTwitchUserID,
                .conditions =
                    {
                        {
                            "broadcaster_user_id",
                            roomId,
                        },
                        {
                            "moderator_user_id",
                            currentTwitchUserID,
                        },
                    },
            });
        this->eventSubSuspiciousUserUpdateHandle =
            getApp()->getEventSub()->subscribe(eventsub::SubscriptionRequest{
                .subscriptionType = "channel.suspicious_user.update",
                .subscriptionVersion = "1",
                .ownerTwitchUserID = currentTwitchUserID,
                .conditions =
                    {
                        {
                            "broadcaster_user_id",
                            roomId,
                        },
                        {
                            "moderator_user_id",
                            currentTwitchUserID,
                        },
                    },
            });

        this->eventSubChannelChatUserMessageHoldHandle.reset();
        this->eventSubChannelChatUserMessageUpdateHandle.reset();
    }
    else
    {
        this->eventSubChannelModerateHandle.reset();
        this->eventSubAutomodMessageHoldHandle.reset();
        this->eventSubAutomodMessageUpdateHandle.reset();
        this->eventSubSuspiciousUserMessageHandle.reset();
        this->eventSubSuspiciousUserUpdateHandle.reset();

        this->eventSubChannelChatUserMessageHoldHandle =
            getApp()->getEventSub()->subscribe(eventsub::SubscriptionRequest{
                .subscriptionType = "channel.chat.user_message_hold",
                .subscriptionVersion = "1",
                .ownerTwitchUserID = currentTwitchUserID,
                .conditions =
                    {
                        {
                            "broadcaster_user_id",
                            roomId,
                        },
                        {
                            "user_id",
                            currentTwitchUserID,
                        },
                    },
            });

        this->eventSubChannelChatUserMessageUpdateHandle =
            getApp()->getEventSub()->subscribe(eventsub::SubscriptionRequest{
                .subscriptionType = "channel.chat.user_message_update",
                .subscriptionVersion = "1",
                .ownerTwitchUserID = currentTwitchUserID,
                .conditions =
                    {
                        {
                            "broadcaster_user_id",
                            roomId,
                        },
                        {
                            "user_id",
                            currentTwitchUserID,
                        },
                    },
            });
    }
}

void TwitchChannel::refreshChatters()
{
    // helix endpoint only works for mods
    if (!this->hasModRights())
    {
        return;
    }

    // setting?
    const auto streamStatus = this->accessStreamStatus();
    const auto viewerCount = static_cast<int>(streamStatus->viewerCount);
    if (getSettings()->onlyFetchChattersForSmallerStreamers)
    {
        if (streamStatus->live &&
            viewerCount > getSettings()->smallStreamerLimit)
        {
            return;
        }
    }

    // Get chatter list via helix api
    getHelix()->getChatters(
        this->roomId(),
        getApp()->getAccounts()->twitch.getCurrent()->getUserId(),
        MAX_CHATTERS_TO_FETCH,
        [this, weak = weakOf<Channel>(this)](auto result) {
            if (auto shared = weak.lock())
            {
                this->updateOnlineChatters(result.chatters);
                this->chatterCount_ = result.total;
            }
        },
        // Refresh chatters should only be used when failing silently is an option
        [](auto error, auto message) {
            (void)error;
            (void)message;
        });
}

void TwitchChannel::addReplyThread(const std::shared_ptr<MessageThread> &thread)
{
    this->threads_[thread->rootId()] = thread;
}

const std::unordered_map<QString, std::weak_ptr<MessageThread>> &
    TwitchChannel::threads() const
{
    return this->threads_;
}

std::shared_ptr<MessageThread> TwitchChannel::getOrCreateThread(
    const MessagePtr &message)
{
    assert(message != nullptr);

    auto threadIt = this->threads_.find(message->id);
    if (threadIt != this->threads_.end() && !threadIt->second.expired())
    {
        return threadIt->second.lock();
    }

    auto thread = std::make_shared<MessageThread>(message);
    this->addReplyThread(thread);
    return thread;
}

void TwitchChannel::cleanUpReplyThreads()
{
    for (auto it = this->threads_.begin(), last = this->threads_.end();
         it != last;)
    {
        bool doErase = true;
        if (auto thread = it->second.lock())
        {
            doErase = thread->liveCount() == 0;
        }

        if (doErase)
        {
            it = this->threads_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void TwitchChannel::refreshBadges()
{
    if (this->roomId().isEmpty())
    {
        return;
    }

    getHelix()->getChannelBadges(
        this->roomId(),
        // successCallback
        [this, weak = weakOf<Channel>(this)](const auto &channelBadges) {
            auto shared = weak.lock();
            if (!shared)
            {
                // The channel has been closed inbetween us making the request and the request finishing
                return;
            }

            this->addTwitchBadgeSets(channelBadges);
        },
        // failureCallback
        [this, weak = weakOf<Channel>(this)](auto error, auto message) {
            auto shared = weak.lock();
            if (!shared)
            {
                // The channel has been closed inbetween us making the request and the request finishing
                return;
            }

            QString errorMessage("Failed to load channel badges - ");

            switch (error)
            {
                case HelixGetChannelBadgesError::Forwarded: {
                    errorMessage += message;
                }
                break;

                // This would most likely happen if the service is down, or if the JSON payload returned has changed format
                case HelixGetChannelBadgesError::Unknown: {
                    errorMessage += "An unknown error has occurred.";
                }
                break;
            }

            this->addSystemMessage(errorMessage);
        });
}

void TwitchChannel::addTwitchBadgeSets(const HelixChannelBadges &channelBadges)
{
    auto badgeSets = this->badgeSets_.access();

    for (const auto &badgeSet : channelBadges.badgeSets)
    {
        const auto &setID = badgeSet.setID;
        for (const auto &version : badgeSet.versions)
        {
            auto emote = Emote{
                .name = EmoteName{},
                .images =
                    ImageSet{
                        Image::fromUrl(version.imageURL1x, 1, BASE_BADGE_SIZE),
                        Image::fromUrl(version.imageURL2x, .5,
                                       BASE_BADGE_SIZE * 2),
                        Image::fromUrl(version.imageURL4x, .25,
                                       BASE_BADGE_SIZE * 4),
                    },
                .tooltip = Tooltip{version.title},
                .homePage = version.clickURL,
            };
            (*badgeSets)[setID][version.id] = std::make_shared<Emote>(emote);
        }
    }
}

void TwitchChannel::refreshCheerEmotes()
{
    getHelix()->getCheermotes(
        this->roomId(),
        [this, weak = weakOf<Channel>(this)](
            const std::vector<HelixCheermoteSet> &cheermoteSets) {
            auto shared = weak.lock();
            if (!shared)
            {
                return;
            }

            this->setCheerEmoteSets(cheermoteSets);
        },
        [] {
            // Failure
        });
}

void TwitchChannel::setCheerEmoteSets(
    const std::vector<HelixCheermoteSet> &cheermoteSets)
{
    std::vector<CheerEmoteSet> emoteSets;

    for (const auto &set : cheermoteSets)
    {
        auto cheerEmoteSet = CheerEmoteSet();
        cheerEmoteSet.regex =
            QRegularExpression("^" + set.prefix + "([1-9][0-9]*)$",
                               QRegularExpression::CaseInsensitiveOption);

        for (const auto &tier : set.tiers)
        {
            CheerEmote cheerEmote;

            cheerEmote.color = QColor(tier.color);
            cheerEmote.minBits = tier.minBits;
            cheerEmote.regex = cheerEmoteSet.regex;

            // TODO(pajlada): We currently hardcode dark here :|
            // We will continue to do so for now since we haven't had to
            // solve that anywhere else

            // Combine the prefix (e.g. BibleThump) with the tier (1, 100 etc.)
            auto emoteTooltip = set.prefix + tier.id + "<br>Twitch Cheer Emote";
            auto makeImageSet = [](const HelixCheermoteImage &image) {
                return ImageSet{
                    Image::fromUrl(image.imageURL1x, 1.0, BASE_BADGE_SIZE),
                    Image::fromUrl(image.imageURL2x, 0.5, BASE_BADGE_SIZE * 2),
                    Image::fromUrl(image.imageURL4x, 0.25, BASE_BADGE_SIZE * 4),
                };
            };
            cheerEmote.animatedEmote = std::make_shared<Emote>(Emote{
                .name = EmoteName{u"cheer emote"_s},
                .images = makeImageSet(tier.darkAnimated),
                .tooltip = Tooltip{emoteTooltip},
                .homePage = Url{},
            });
            cheerEmote.staticEmote = std::make_shared<Emote>(Emote{
                .name = EmoteName{u"cheer emote"_s},
                .images = makeImageSet(tier.darkStatic),
                .tooltip = Tooltip{emoteTooltip},
                .homePage = Url{},
            });

            cheerEmoteSet.cheerEmotes.emplace_back(std::move(cheerEmote));
        }

        // Sort cheermotes by cost
        std::sort(cheerEmoteSet.cheerEmotes.begin(),
                  cheerEmoteSet.cheerEmotes.end(),
                  [](const auto &lhs, const auto &rhs) {
                      return lhs.minBits > rhs.minBits;
                  });

        emoteSets.emplace_back(std::move(cheerEmoteSet));
    }

    *this->cheerEmoteSets_.access() = std::move(emoteSets);
}

void TwitchChannel::createClip(const QString &title,
                               const std::optional<int> duration)
{
    if (!this->isLive())
    {
        this->addSystemMessage(
            "Cannot create clip while the channel is offline!");
        return;
    }

    // timer has never started, proceed and start it
    if (!this->clipCreationTimer_.isValid())
    {
        this->clipCreationTimer_.start();
    }
    else if (this->clipCreationTimer_.elapsed() < CLIP_CREATION_COOLDOWN ||
             this->isClipCreationInProgress)
    {
        return;
    }

    this->addSystemMessage("Creating clip...");
    this->isClipCreationInProgress = true;

    getHelix()->createClip(
        this->roomId(), title, duration,
        // successCallback
        [this](const HelixClip &clip) {
            MessageBuilder builder;
            QString text(
                "Clip created! Copy link to clipboard or edit it in browser.");
            builder.message().messageText = text;
            builder.message().searchText = text;
            builder.message().flags.set(MessageFlag::System);

            builder.emplace<TimestampElement>();
            // text
            builder.emplace<TextElement>("Clip created!",
                                         MessageElementFlag::Text,
                                         MessageColor::System);
            // clip link
            builder
                .emplace<TextElement>("Copy link to clipboard",
                                      MessageElementFlag::Text,
                                      MessageColor::Link)
                ->setLink(Link(Link::CopyToClipboard, CLIPS_LINK.arg(clip.id)));
            // separator text
            builder.emplace<TextElement>("or", MessageElementFlag::Text,
                                         MessageColor::System);
            // edit link
            builder
                .emplace<TextElement>("edit it in browser.",
                                      MessageElementFlag::Text,
                                      MessageColor::Link)
                ->setLink(Link(Link::Url, clip.editUrl));

            this->addMessage(builder.release(), MessageContext::Original);
        },
        // failureCallback
        [this](auto error, auto errorMessage) {
            MessageBuilder builder;
            QString text;
            builder.message().flags.set(MessageFlag::System);

            builder.emplace<TimestampElement>();

            switch (error)
            {
                case HelixClipError::ClipsUnavailable: {
                    builder.emplace<TextElement>(
                        CLIPS_FAILURE_CLIPS_UNAVAILABLE_TEXT.arg(errorMessage),
                        MessageElementFlag::Text, MessageColor::System);
                    text =
                        CLIPS_FAILURE_CLIPS_UNAVAILABLE_TEXT.arg(errorMessage);
                }
                break;

                case HelixClipError::ClipsDisabled: {
                    builder.emplace<TextElement>(
                        CLIPS_FAILURE_CLIPS_DISABLED_TEXT,
                        MessageElementFlag::Text, MessageColor::System);
                    text = CLIPS_FAILURE_CLIPS_DISABLED_TEXT;
                }
                break;

                case HelixClipError::ClipsRestricted: {
                    builder.emplace<TextElement>(
                        CLIPS_FAILURE_CLIPS_RESTRICTED_TEXT,
                        MessageElementFlag::Text, MessageColor::System);
                    text = CLIPS_FAILURE_CLIPS_RESTRICTED_TEXT;
                }
                break;

                case HelixClipError::ClipsRestrictedCategory: {
                    builder.emplace<TextElement>(
                        CLIPS_FAILURE_CLIPS_RESTRICTED_CATEGORY_TEXT,
                        MessageElementFlag::Text, MessageColor::System);
                    text = CLIPS_FAILURE_CLIPS_RESTRICTED_CATEGORY_TEXT;
                }
                break;

                case HelixClipError::UserNotAuthenticated: {
                    builder.emplace<TextElement>(
                        CLIPS_FAILURE_NOT_AUTHENTICATED_TEXT,
                        MessageElementFlag::Text, MessageColor::System);
                    builder
                        .emplace<TextElement>(LOGIN_PROMPT_TEXT,
                                              MessageElementFlag::Text,
                                              MessageColor::Link)
                        ->setLink(ACCOUNTS_LINK);
                    text = QString("%1 %2").arg(
                        CLIPS_FAILURE_NOT_AUTHENTICATED_TEXT,
                        LOGIN_PROMPT_TEXT);
                }
                break;

                // This would most likely happen if the service is down, or if the JSON payload returned has changed format
                case HelixClipError::Unknown:
                default: {
                    builder.emplace<TextElement>(
                        CLIPS_FAILURE_UNKNOWN_ERROR_TEXT.arg(errorMessage),
                        MessageElementFlag::Text, MessageColor::System);
                    text = CLIPS_FAILURE_UNKNOWN_ERROR_TEXT.arg(errorMessage);
                }
                break;
            }

            builder.message().messageText = text;
            builder.message().searchText = text;

            this->addMessage(builder.release(), MessageContext::Original);
        },
        // finallyCallback - this will always execute, so clip creation won't ever be stuck
        [this] {
            this->clipCreationTimer_.restart();
            this->isClipCreationInProgress = false;
        });
}

void TwitchChannel::deleteMessagesAs(const QString &messageID,
                                     TwitchAccount *moderator)
{
    getHelix()->deleteChatMessages(
        this->roomId(), moderator->getUserId(), messageID,
        []() {
            // Success handling, we do nothing: IRC/pubsub will dispatch the correct
            // events to update state for us.
        },
        [lifetime{this->weak_from_this()}, messageID](auto error,
                                                      const auto &message) {
            auto self =
                std::dynamic_pointer_cast<TwitchChannel>(lifetime.lock());
            if (!self)
            {
                return;
            }

            QString errorMessage = QString("Failed to delete chat messages - ");

            switch (error)
            {
                case HelixDeleteChatMessagesError::UserMissingScope: {
                    errorMessage +=
                        "Missing required scope. Re-login with your "
                        "account and try again.";
                }
                break;

                case HelixDeleteChatMessagesError::UserNotAuthorized: {
                    errorMessage +=
                        "you don't have permission to perform that action.";
                }
                break;

                case HelixDeleteChatMessagesError::MessageUnavailable: {
                    // Override default message prefix to match with IRC message format
                    errorMessage =
                        QString("The message %1 does not exist, was deleted, "
                                "or is too old to be deleted.")
                            .arg(messageID);
                }
                break;

                case HelixDeleteChatMessagesError::UserNotAuthenticated: {
                    errorMessage += "you need to re-authenticate.";
                }
                break;

                case HelixDeleteChatMessagesError::Forwarded: {
                    errorMessage += message;
                }
                break;

                case HelixDeleteChatMessagesError::Unknown:
                default: {
                    errorMessage += "An unknown error has occurred.";
                }
                break;
            }

            self->addSystemMessage(errorMessage);
        });
}

std::optional<EmotePtr> TwitchChannel::twitchBadge(const QString &set,
                                                   const QString &version) const
{
    auto badgeSets = this->badgeSets_.access();
    auto it = badgeSets->find(set);
    if (it != badgeSets->end())
    {
        auto it2 = it->second.find(version);
        if (it2 != it->second.end())
        {
            return it2->second;
        }
    }
    return std::nullopt;
}

std::vector<FfzBadges::Badge> TwitchChannel::ffzChannelBadges(
    const QString &userID) const
{
    this->tgFfzChannelBadges_.guard();

    auto it = this->ffzChannelBadges_.find(userID);
    if (it == this->ffzChannelBadges_.end())
    {
        return {};
    }

    std::vector<FfzBadges::Badge> badges;

    const auto *ffzBadges = getApp()->getFfzBadges();

    for (const auto &badgeID : it->second)
    {
        auto badge = ffzBadges->getBadge(badgeID);
        if (badge.has_value())
        {
            badges.emplace_back(*badge);
        }
    }

    return badges;
}

void TwitchChannel::setFfzChannelBadges(FfzChannelBadgeMap map)
{
    this->tgFfzChannelBadges_.guard();
    this->ffzChannelBadges_ = std::move(map);
}

std::optional<EmotePtr> TwitchChannel::ffzCustomModBadge() const
{
    return this->ffzCustomModBadge_.get();
}

std::optional<EmotePtr> TwitchChannel::ffzCustomVipBadge() const
{
    return this->ffzCustomVipBadge_.get();
}

void TwitchChannel::setFfzCustomModBadge(std::optional<EmotePtr> badge)
{
    this->ffzCustomModBadge_.set(std::move(badge));
}

void TwitchChannel::setFfzCustomVipBadge(std::optional<EmotePtr> badge)
{
    this->ffzCustomVipBadge_.set(std::move(badge));
}

std::optional<CheerEmote> TwitchChannel::cheerEmote(const QString &string) const
{
    auto sets = this->cheerEmoteSets_.access();
    for (const auto &set : *sets)
    {
        auto match = set.regex.match(string);
        if (!match.hasMatch())
        {
            continue;
        }
        QString amount = match.captured(1);
        bool ok = false;
        int bitAmount = amount.toInt(&ok);
        if (!ok)
        {
            qCDebug(chatterinoTwitch)
                << "Error parsing bit amount in cheerEmote";
        }
        for (const auto &emote : set.cheerEmotes)
        {
            if (bitAmount >= emote.minBits)
            {
                return emote;
            }
        }
    }
    return std::nullopt;
}

void TwitchChannel::updateBttvActivity()
{
    if (!getApp()->getBttvLiveUpdates())
    {
        return;
    }

    auto now = QDateTime::currentDateTimeUtc();
    if (this->nextBttvActivity_.isValid() && now < this->nextBttvActivity_)
    {
        return;
    }
    this->nextBttvActivity_ = now.addSecs(60);

    qCDebug(chatterinoBttv) << "Sending activity in" << this->getName();

    auto acc = getApp()->getAccounts()->twitch.getCurrent();
    if (acc->isAnon())
    {
        return;
    }
    getApp()->getBttvLiveUpdates()->broadcastMe(this->roomId(),
                                                acc->getUserId());
}

void TwitchChannel::updateSevenTVActivity()
{
    static const QString seventvActivityUrl =
        QStringLiteral("https://7tv.io/v3/users/%1/presences");

    const auto currentSeventvUserID =
        getApp()->getAccounts()->twitch.getCurrent()->getSeventvUserID();
    if (currentSeventvUserID.isEmpty())
    {
        return;
    }

    if (!getSettings()->enableSevenTVEventAPI ||
        !getSettings()->sendSevenTVActivity)
    {
        return;
    }

    if (this->nextSeventvActivity_.isValid() &&
        QDateTime::currentDateTimeUtc() < this->nextSeventvActivity_)
    {
        return;
    }
    // Make sure to not send activity again before receiving the response
    this->nextSeventvActivity_ = this->nextSeventvActivity_.addSecs(300);

    qCDebug(chatterinoSeventv) << "Sending activity in" << this->getName();

    getApp()->getSeventvAPI()->updatePresence(
        this->roomId(), currentSeventvUserID,
        [chan = weakOf<Channel>(this)]() {
            const auto self =
                std::dynamic_pointer_cast<TwitchChannel>(chan.lock());
            if (!self)
            {
                return;
            }
            self->nextSeventvActivity_ =
                QDateTime::currentDateTimeUtc().addSecs(60);
        },
        [](const auto &result) {
            qCDebug(chatterinoSeventv)
                << "Failed to update 7TV activity:" << result.formatError();
        });
}

void TwitchChannel::listenSevenTVCosmetics() const
{
    if (getApp()->getSeventvEventAPI())
    {
        getApp()->getSeventvEventAPI()->subscribeTwitchChannel(this->roomId());
    }
}

void TwitchChannel::syncSendWaitTimer()
{
    auto now = std::chrono::steady_clock::now();
    const auto remaining =
        this->sendWaitEnd_.has_value()
            ? std::chrono::duration_cast<std::chrono::seconds>(
                  this->sendWaitEnd_.value() - now)
            : 0s;
    if (remaining <= 0s)
    {
        this->sendWaitTimer_.stop();
        this->sendWaitUpdate.invoke("");
    }
    else
    {
        this->sendWaitUpdate.invoke(formatTime(remaining, 2));
    }
}

void TwitchChannel::setSendWait(int seconds)
{
    if (seconds <= 0)
    {
        if (this->sendWaitEnd_.has_value())
        {
            this->sendWaitEnd_ = std::nullopt;
            this->syncSendWaitTimer();
        }
        return;
    }
    this->sendWaitEnd_ =
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    if (!this->sendWaitTimer_.isActive())
    {
        this->sendWaitTimer_.start(1s);
        this->syncSendWaitTimer();
    }
}

bool TwitchChannel::isLoadingRecentMessages() const
{
    return this->loadingRecentMessages_.test();
}

}  // namespace chatterino
