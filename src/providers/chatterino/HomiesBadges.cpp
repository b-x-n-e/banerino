// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/chatterino/HomiesBadges.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>

namespace chatterino {

HomiesBadges::HomiesBadges()
{
    this->loadHomiesBadges();
}

std::vector<EmotePtr> HomiesBadges::getBadges(const UserId &id)
{
    std::shared_lock lock(this->mutex_);

    std::vector<EmotePtr> userBadges;
    auto it = this->badgeMap.find(id.string);
    if (it != this->badgeMap.end())
    {
        for (int index : it->second)
        {
            if (index >= 0 && index < static_cast<int>(this->emotes.size()))
            {
                userBadges.push_back(this->emotes[index]);
            }
        }
    }
    return userBadges;
}

void HomiesBadges::loadHomiesBadges()
{
    static const QString userAgent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36";

    auto parseBadges = [this](const NetworkResult &result, const QString &sourceName) {
        auto jsonRoot = result.parseJson();
        auto badgesValue = jsonRoot.value("badges");
        if (!badgesValue.isArray())
        {
            qDebug() << "[Homies] Unexpected payload shape from" << sourceName;
            return;
        }

        std::unique_lock lock(this->mutex_);

        for (const auto &badgeVal : badgesValue.toArray())
        {
            auto jsonBadge = badgeVal.toObject();
            auto tooltip = jsonBadge.value("tooltip").toString();
            if (tooltip.isEmpty())
            {
                continue;
            }

            constexpr QSize baseSize(18, 18);
            auto emote = std::make_shared<const Emote>(Emote{
                .name = EmoteName{u"homies:" % tooltip},
                .images =
                    ImageSet{
                        Image::fromUrl(Url{jsonBadge.value("image1").toString()},
                                       1.0, baseSize),
                        Image::fromUrl(Url{jsonBadge.value("image2").toString()},
                                       0.5, baseSize * 2),
                        Image::fromUrl(Url{jsonBadge.value("image3").toString()},
                                       0.25, baseSize * 4),
                    },
                .tooltip = Tooltip{tooltip},
                .homePage = Url{},
            });

            int badgeIndex = -1;
            for (size_t i = 0; i < this->emotes.size(); ++i)
            {
                if (this->emotes[i]->tooltip.string == tooltip)
                {
                    badgeIndex = static_cast<int>(i);
                    break;
                }
            }

            if (badgeIndex == -1)
            {
                this->emotes.push_back(emote);
                badgeIndex = static_cast<int>(this->emotes.size() - 1);
            }

            // Check Structure A: "users" array
            if (jsonBadge.contains("users") && jsonBadge.value("users").isArray())
            {
                for (const auto &userVal : jsonBadge.value("users").toArray())
                {
                    auto userId = userVal.toString();
                    if (!userId.isEmpty())
                    {
                        auto &userBadges = this->badgeMap[userId];
                        if (std::find(userBadges.begin(), userBadges.end(),
                                      badgeIndex) == userBadges.end())
                        {
                            userBadges.push_back(badgeIndex);
                        }
                    }
                }
            }

            // Check Structure B: "userId" string
            if (jsonBadge.contains("userId"))
            {
                auto userId = jsonBadge.value("userId").toString();
                if (!userId.isEmpty())
                {
                    auto &userBadges = this->badgeMap[userId];
                    if (std::find(userBadges.begin(), userBadges.end(),
                                  badgeIndex) == userBadges.end())
                    {
                        userBadges.push_back(badgeIndex);
                    }
                }
            }
        }
        qDebug() << "[Homies] Loaded badges from" << sourceName;
    };

    // 1. chatterinohomies.com API
    NetworkRequest(QUrl("https://chatterinohomies.com/api/badges/list"))
        .concurrent()
        .header("User-Agent", userAgent)
        .onSuccess([parseBadges](auto result) {
            parseBadges(result, "chatterinohomies.com");
        })
        .onError([](auto result) {
            qDebug() << "[Homies] Failed to load chatterinohomies.com badges:"
                     << result.status();
        })
        .execute();

    // 2. itzalex.github.io badges 1
    NetworkRequest(QUrl("https://itzalex.github.io/badges"))
        .concurrent()
        .header("User-Agent", userAgent)
        .onSuccess([parseBadges](auto result) {
            parseBadges(result, "itzalex.github.io/badges");
        })
        .onError([](auto result) {
            qDebug() << "[Homies] Failed to load itzalex.github.io/badges:"
                     << result.status();
        })
        .execute();

    // 3. itzalex.github.io badges 2
    NetworkRequest(QUrl("https://itzalex.github.io/badges2"))
        .concurrent()
        .header("User-Agent", userAgent)
        .onSuccess([parseBadges](auto result) {
            parseBadges(result, "itzalex.github.io/badges2");
        })
        .onError([](auto result) {
            qDebug() << "[Homies] Failed to load itzalex.github.io/badges2:"
                     << result.status();
        })
        .execute();
}

}  // namespace chatterino
