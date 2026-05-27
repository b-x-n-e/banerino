// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"

#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class HomiesBadges
{
public:
    /**
     * Makes a network request to load Chatterino Homies badges
     */
    HomiesBadges();

    /**
     * Returns all Homies badges for the given user
     */
    std::vector<EmotePtr> getBadges(const UserId &id);

private:
    void loadHomiesBadges();

    std::shared_mutex mutex_;

    /**
     * Maps Twitch user IDs to their list of badge indexes
     * Guarded by mutex_
     */
    std::unordered_map<QString, std::vector<int>> badgeMap;

    /**
     * Keeps a list of badges.
     * Indexes in here are referred to by badgeMap
     */
    std::vector<EmotePtr> emotes;
};

}  // namespace chatterino
