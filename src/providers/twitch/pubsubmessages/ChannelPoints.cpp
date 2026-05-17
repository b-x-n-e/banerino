// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/pubsubmessages/ChannelPoints.hpp"

#include "util/QMagicEnum.hpp"

namespace chatterino {

PubSubCommunityPointsChannelV1Message::PubSubCommunityPointsChannelV1Message(
    const QJsonObject &root)
    : typeString(root.value("type").toString())
    , data(root.value("data").toObject())
{
    auto oType = qmagicenum::enumCast<Type>(this->typeString);
    if (oType.has_value())
    {
        this->type = oType.value();
    }
}

PubSubCommunityPointsUserV1Message::PubSubCommunityPointsUserV1Message(
    const QJsonObject &root)
    : typeString(root.value("type").toString())
    , data(root.value("data").toObject())
{
    auto oType = qmagicenum::enumCast<Type>(this->typeString);
    if (oType.has_value())
    {
        this->type = oType.value();
    }

    if (this->data.contains("balance"))
    {
        this->balance = this->data.value("balance").toObject().value("balance").toInt(-1);
    }

    if (this->data.contains("channel_id"))
    {
        this->channelId = this->data.value("channel_id").toString();
    }
}

}  // namespace chatterino
