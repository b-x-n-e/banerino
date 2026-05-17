#pragma once

#include "providers/seventv/paints/Paint.hpp"

#include <QJsonArray>
#include <QString>

#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace chatterino {

namespace seventv::eventapi {
struct TwitchUser;
struct KickUser;
using User = std::variant<TwitchUser, KickUser>;
}  // namespace seventv::eventapi

class SeventvPaints
{
public:
    SeventvPaints();

    void addPaint(const QJsonObject &paintJson);
    void assignPaintToUsers(const QString &paintID,
                            std::span<const seventv::eventapi::User> users);
    void clearPaintFromUsers(const QString &paintID,
                             std::span<const seventv::eventapi::User> users);

    std::shared_ptr<Paint> getPaint(const QString &userName, bool kick) const;


    /// Look up a user's paint by their Twitch user ID and assign it
    /// to their username. This is a no-op if the user already has a
    /// paint assigned or has been looked up before.
    void resolveUserPaint(const QString &twitchUserID,
                          const QString &twitchUserName);

private:
    // Mutex for both `paintMap_` and `knownPaints_`
    mutable std::shared_mutex mutex_;

    // user-name => paint
    std::unordered_map<QString, std::shared_ptr<Paint>> kickPaintMap_;
    // user-name => paint
    std::unordered_map<QString, std::shared_ptr<Paint>> twitchPaintMap_;
    // paint-id => paint
    std::unordered_map<QString, std::shared_ptr<Paint>> knownPaints_;

    // Set of Twitch user IDs we've already looked up (to avoid repeat requests)
    std::unordered_set<QString> lookedUpUsers_;
    std::mutex lookupMutex_;
};

}  // namespace chatterino
