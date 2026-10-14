#pragma once
// Bots for the multiplayer: test clients the server adds, which a brain of
// this runtime's own plays as a person would (bots/brain.cpp), where the
// title's own test clients only wander and fire at random.
//
// The title still has everything a test client needs: SV_AddTestClient, the
// server frame that runs each one's usercmd, and the team and class menus a
// bot answers as a player does. The runtime adds the bots up to the count the
// settings menu asks for while this machine hosts a match, and writes every
// bot's usercmd each server frame from what it sees.
//
// Navigation is better with a map's waypoints: Bot Warfare's CSV files
// (<map>_wp.csv), read from bots/waypoints/ in the player's folder when they
// are there. Without them a bot finds its way by line of sight alone.
#include <cstdint>

namespace bots
{
    // From the settings menu (and MW2_BOTS, MW2_BOT_SKILL, MW2_BOT_TEAMS).
    enum class Skill { Recruit, Regular, Hardened, Veteran };
    enum class Teams { Against, Both };
    void SetCount(uint32_t count);
    void SetSkill(Skill skill);
    void SetTeams(Teams teams);
    uint32_t Count();
}
