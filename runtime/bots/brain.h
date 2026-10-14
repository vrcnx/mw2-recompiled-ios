#pragma once
// A bot's mind: what it sees of the players, and the usercmd it would press.
// Nothing here knows the engine; bots.cpp gathers the world and runs the
// command.
#include <cstdint>
#include <functional>
#include <vector>

namespace bots::brain
{
    struct Vec3 { float x = 0, y = 0, z = 0; };

    enum class Skill { Recruit, Regular, Hardened, Veteran };

    struct Player
    {
        uint32_t num = 0;
        bool bot = false, alive = false, onGround = true;
        uint32_t team = 0;      // 0 free-for-all, 1 axis, 2 allies, 3 spectating
        int health = 0;
        Vec3 origin;            // at the feet
        Vec3 view;              // pitch, yaw, roll
    };

    struct World
    {
        std::vector<Player> players;
        double time = 0;        // seconds
        Skill skill = Skill::Regular;
        // A clear line between two points, `pass` the entity the line ignores.
        std::function<bool(const Vec3&, const Vec3&, uint32_t pass)> visible;
    };

    struct Command
    {
        float pitch = 0, yaw = 0;    // the view wanted, in degrees
        int8_t forward = 0, right = 0;
        uint32_t buttons = 0;
    };

    Command Think(uint32_t num, const World& world);
    // What it is doing, for a log line.
    const char* Describe(uint32_t num);
    // Dead, or between lives: the next life starts with a clean mind.
    void Dead(uint32_t num);
    // Once a server frame, before the bots think: where every player went,
    // for the routes learned on a map without waypoints.
    void Observe(const World& world);
    // A new map: its waypoints if there are any, and every bot starts afresh.
    void NewMap(const char* map);
}
