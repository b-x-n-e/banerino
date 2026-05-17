#pragma once

#include <QDateTime>
#include <QString>

#include <optional>

namespace chatterino {

struct TwitchPinnedMessage {
    QString pinId;        // the pinnedChatMessage node ID (for unpin)
    QString messageId;    // original message ID
    QString text;         // message content
    QString senderId;     // sender's user ID
    QString senderLogin;  // who wrote the message
    QString senderName;   // display name
    QString chatColor;    // sender's twitch color
    QDateTime sentAt;     // when the message was sent
    QDateTime pinnedAt;   // when the message was pinned
    QString pinnerLogin;  // who pinned the message
    QString pinnerName;   // pinner display name

    bool operator==(const TwitchPinnedMessage &other) const
    {
        return this->pinId == other.pinId && this->text == other.text;
    }

    bool operator!=(const TwitchPinnedMessage &other) const
    {
        return !(*this == other);
    }
};

}  // namespace chatterino
