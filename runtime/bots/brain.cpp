// How a bot plays: it sees what is in front of it and in the open, takes a
// moment to react, turns onto a target no faster than a hand could and not
// quite on it, closes the error while it tracks, and fires once it is on.
// In a fight it strafes, crouches at range and knifes up close; out of one it
// goes where the fight is -- an enemy's last known position, or where the
// other team is -- along the map's waypoints when there are any, by line of
// sight when there are not, and gets itself unstuck when a wall says no.
#include "brain.h"
#include "../log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <queue>
#include <sstream>
#include <string>

namespace
{
    using bots::brain::Vec3;
    using bots::brain::Player;
    using bots::brain::World;
    using bots::brain::Command;
    using bots::brain::Skill;

    constexpr float kPi = 3.14159265358979f;
    constexpr uint32_t kFire = 0x1, kSprint = 0x2, kMelee = 0x4, kReload = 0x10, kCrouch = 0x200,
                       kJump = 0x400, kAds = 0x800;

    Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    float Length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
    float Flat(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
    Vec3 Up(Vec3 v, float z) { return { v.x, v.y, v.z + z }; }

    float Wrap(float a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }
    float YawTo(Vec3 d) { return std::atan2(d.y, d.x) * 180.0f / kPi; }
    // Positive pitch looks down, as the engine counts it.
    float PitchTo(Vec3 d) { return -std::atan2(d.z, Flat(d)) * 180.0f / kPi; }

    struct Params
    {
        double reaction;      // seconds from seeing to firing
        float turnRate;       // degrees a second
        float error;          // degrees of aim error when a target is first taken
        float fov;            // half the angle it notices things in
        float range;          // how far it can pick a player out
        float headHeight;     // where it aims, above the feet
    };
    Params ParamsFor(Skill skill)
    {
        switch (skill)
        {
        case Skill::Recruit:  return { 0.75, 200, 7.0f, 55, 2400, 44 };
        case Skill::Regular:  return { 0.45, 340, 4.0f, 65, 3400, 48 };
        case Skill::Hardened: return { 0.28, 520, 2.2f, 75, 4500, 54 };
        case Skill::Veteran:  return { 0.17, 760, 1.2f, 85, 6000, 58 };
        }
        return ParamsFor(Skill::Regular);
    }

    // ---- waypoints (Bot Warfare's CSV) ------------------------------------
    struct Node { Vec3 at; std::vector<int> links; bool crouch = false, climb = false; };
    std::vector<Node> g_nodes;
    std::string g_map;
    bool g_learned = false;          // the nodes are learned ones, not a file's
    size_t g_savedNodes = 0;
    double g_savedAt = 0;
    int g_lastNode[18];
    Vec3 g_lastSpot[18];

    void LoadWaypoints(const std::string& map)
    {
        g_nodes.clear();
        const std::string path = "bots/waypoints/" + map + "_wp.csv";
        g_learned = false;
        std::ifstream file(path);
        if (!file)
        {
            // The routes learned on it before, or none yet.
            g_learned = true;
            file.open("bots/learned/" + map + "_wp.csv");
            if (!file)
            {
                LOGI("bots: no waypoints for %s (%s); the bots learn the map as it is played", map.c_str(),
                     path.c_str());
                return;
            }
        }
        std::string line;
        std::getline(file, line);   // the count
        while (std::getline(file, line))
        {
            std::stringstream fields(line);
            std::string position, links, type;
            std::getline(fields, position, ',');
            std::getline(fields, links, ',');
            std::getline(fields, type, ',');
            Node node;
            if (std::sscanf(position.c_str(), "%f %f %f", &node.at.x, &node.at.y, &node.at.z) != 3) continue;
            std::stringstream ids(links);
            for (int id; ids >> id;) node.links.push_back(id);
            node.crouch = type == "crouch";
            node.climb = type == "climb";
            g_nodes.push_back(std::move(node));
        }
        for (Node& node : g_nodes)
            node.links.erase(std::remove_if(node.links.begin(), node.links.end(),
                                            [](int id) { return id < 0 || id >= int(g_nodes.size()); }),
                             node.links.end());
        LOGI("bots: %zu %s for %s", g_nodes.size(), g_learned ? "learned waypoints" : "waypoints", map.c_str());
    }

    // ---- routes learned from where players walk -----------------------------
    // Without a map's waypoints, the places people go are the waypoints: a
    // node every so often along each walk, linked to the one before. Kept per
    // map in bots/learned/, Bot Warfare's format, so a map is learned once.

    void SaveLearned()
    {
        if (!g_learned || g_map.empty() || g_nodes.size() == g_savedNodes) return;
        std::error_code ec;
        const std::filesystem::path folder = "bots/learned";
        std::filesystem::create_directories(folder, ec);
        const std::filesystem::path path = folder / (g_map + "_wp.csv");
        const std::filesystem::path partial = path.string() + ".partial";
        {
            std::ofstream out(partial, std::ios::trunc);
            out << g_nodes.size() << "\n";
            for (const Node& node : g_nodes)
            {
                out << node.at.x << " " << node.at.y << " " << node.at.z << ",";
                for (size_t i = 0; i < node.links.size(); i++) out << (i ? " " : "") << node.links[i];
                out << "," << (node.crouch ? "crouch" : "stand") << ",,,\n";
            }
        }
        std::filesystem::rename(partial, path, ec);
        g_savedNodes = g_nodes.size();
    }

    void Link(int a, int b)
    {
        if (a == b || a < 0 || b < 0) return;
        auto& links = g_nodes[size_t(a)].links;
        if (std::find(links.begin(), links.end(), b) == links.end()) links.push_back(b);
    }

    void Learn(const World& world)
    {
        static double told = 0;
        static const bool logging = std::getenv("MW2_BOT_LOG") != nullptr;
        if (logging && world.time - told > 10)
        {
            told = world.time;
            LOGI("bots: learning %s: %d, %zu nodes, %zu players", g_map.c_str(), int(g_learned), g_nodes.size(),
                 world.players.size());
        }
        if (!g_learned) return;
        for (const Player& p : world.players)
        {
            if (p.num >= 18) continue;
            if (!p.alive) { g_lastNode[p.num] = -1; continue; }
            if (!p.onGround) continue;
            // A jump of hundreds of units is a respawn, not a walk.
            const bool walked = g_lastNode[p.num] >= 0 && Flat(p.origin - g_lastSpot[p.num]) < 400 &&
                                std::fabs(p.origin.z - g_lastSpot[p.num].z) < 120;
            g_lastSpot[p.num] = p.origin;
            int near = -1;
            float nearest = 1e30f;
            for (int i = 0; i < int(g_nodes.size()); i++)
            {
                const Vec3 d = g_nodes[size_t(i)].at - p.origin;
                if (std::fabs(d.z) > 48) continue;
                const float flat = Flat(d);
                if (flat < nearest) { nearest = flat; near = i; }
            }
            int node = near;
            if (near < 0 || nearest > 128)
            {
                if (g_nodes.size() >= 3000) continue;
                Node made;
                made.at = p.origin;
                g_nodes.push_back(made);
                node = int(g_nodes.size()) - 1;
            }
            else if (nearest > 72 && g_lastNode[p.num] == node) continue;
            if (walked && g_lastNode[p.num] != node)
            {
                Link(g_lastNode[p.num], node);
                Link(node, g_lastNode[p.num]);
            }
            g_lastNode[p.num] = node;
        }
        if (world.time - g_savedAt > 10.0) { SaveLearned(); g_savedAt = world.time; }
    }

    int Nearest(Vec3 at)
    {
        int best = -1;
        float bestScore = 1e30f;
        for (int i = 0; i < int(g_nodes.size()); i++)
        {
            const Vec3 d = g_nodes[size_t(i)].at - at;
            const float score = d.x * d.x + d.y * d.y + 4.0f * d.z * d.z;   // floors apart are far apart
            if (score < bestScore) { bestScore = score; best = i; }
        }
        return best;
    }

    std::vector<int> Route(int from, int to)
    {
        std::vector<int> path;
        if (from < 0 || to < 0) return path;
        const size_t n = g_nodes.size();
        std::vector<float> cost(n, 1e30f);
        std::vector<int> came(n, -1);
        using Item = std::pair<float, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
        cost[size_t(from)] = 0;
        open.push({ 0, from });
        while (!open.empty())
        {
            const int at = open.top().second;
            open.pop();
            if (at == to) break;
            for (int next : g_nodes[size_t(at)].links)
            {
                const float step = cost[size_t(at)] + Length(g_nodes[size_t(next)].at - g_nodes[size_t(at)].at);
                if (step >= cost[size_t(next)]) continue;
                cost[size_t(next)] = step;
                came[size_t(next)] = at;
                open.push({ step + Length(g_nodes[size_t(to)].at - g_nodes[size_t(next)].at), next });
            }
        }
        if (came[size_t(to)] < 0 && from != to) return path;
        for (int at = to; at != -1; at = came[size_t(at)]) { path.push_back(at); if (at == from) break; }
        std::reverse(path.begin(), path.end());
        return path;
    }

    // ---- a bot ---------------------------------------------------------------
    struct Detour { int side = 0; double until = 0; };
    struct Mind
    {
        bool alive = false;
        double last = 0;                 // the last think
        uint32_t rng = 0;
        // The target.
        uint32_t target = UINT32_MAX;
        double seenAt = 0, acquiredAt = 0;
        Vec3 lastSeen;
        bool hunting = false;
        double huntUntil = 0;
        // The aim, which turns toward what is wanted at a hand's speed.
        float pitch = 0, yaw = 0;
        bool aimed = false;
        float errPitch = 0, errYaw = 0;
        double errAt = 0;
        // The way.
        std::vector<int> path;
        size_t step = 0;
        Vec3 goal;
        bool hasGoal = false;
        double goalAt = 0;
        // Getting unstuck.
        Vec3 checkAt;
        double checkTime = 0;
        int stuck = 0;
        double jumpUntil = 0, sideUntil = 0;
        int side = 1;
        // In a fight.
        double strafeUntil = 0;
        int strafe = 1;
        double crouchUntil = 0;
        double lastReload = 0;
        uint32_t fireTicks = 0;
        uint32_t lastButtons = 0;
        uint32_t steerTicks = 0;
        Vec3 steer;
        Detour detour;
    };
    Mind g_minds[18];

    float Random(Mind& m)
    {
        m.rng = m.rng * 1664525u + 1013904223u;
        return float(m.rng >> 8) / float(1u << 24);
    }

    bool Enemy(const Player& me, const Player& other)
    {
        if (other.num == me.num || !other.alive || other.team == 3) return false;
        return me.team == 0 || other.team != me.team;
    }

    const Player* Find(const World& world, uint32_t num)
    {
        for (const Player& p : world.players) if (p.num == num) return &p;
        return nullptr;
    }

    Vec3 Eye(const Player& p) { return Up(p.origin, 60); }

    // Picks a target: the current one while it stays in sight, otherwise the
    // nearest enemy in view and in the open.
    void Look(Mind& m, const Player& me, const World& world, const Params& params)
    {
        const Vec3 eye = Eye(me);
        auto sees = [&](const Player& other, bool current) {
            const Vec3 to = other.origin - me.origin;
            const float distance = Length(to);
            if (distance > params.range) return false;
            // In view, or so near it would be heard; a target already taken is
            // followed out of the corner of the eye.
            const float off = std::fabs(Wrap(YawTo(to) - m.yaw));
            if (!current && off > params.fov && distance > 350) return false;
            if (!world.visible) return false;
            return world.visible(eye, Up(other.origin, 48), me.num) ||
                   world.visible(eye, Up(other.origin, 62), me.num);
        };

        if (m.target != UINT32_MAX)
        {
            const Player* current = Find(world, m.target);
            if (current && Enemy(me, *current) && sees(*current, true))
            {
                m.seenAt = world.time;
                m.lastSeen = current->origin;
                return;
            }
            if (current && current->alive)
            {
                // Out of sight: after it, for a while.
                m.hunting = true;
                m.huntUntil = world.time + 4.0 + 2.0 * Random(m);
            }
            m.target = UINT32_MAX;
        }
        const Player* best = nullptr;
        float bestDistance = 1e30f;
        for (const Player& other : world.players)
        {
            if (!Enemy(me, other)) continue;
            const float distance = Length(other.origin - me.origin);
            if (distance >= bestDistance || !sees(other, false)) continue;
            best = &other;
            bestDistance = distance;
        }
        if (!best) return;
        m.target = best->num;
        m.seenAt = m.acquiredAt = world.time;
        m.lastSeen = best->origin;
        m.hunting = false;
        // A fresh target, a fresh error: a little off, and more so far away.
        const float spread = params.error * (0.6f + 0.8f * Random(m));
        m.errYaw = (Random(m) * 2 - 1) * spread;
        m.errPitch = (Random(m) * 2 - 1) * spread * 0.6f;
        m.errAt = world.time;
    }

    // Toward where it wants to look, no faster than its hand.
    void Turn(Mind& m, float pitch, float yaw, float dt, float rate)
    {
        if (!m.aimed) { m.pitch = pitch; m.yaw = yaw; m.aimed = true; return; }
        const float most = rate * dt;
        const float dy = Wrap(yaw - m.yaw), dp = pitch - m.pitch;
        // Fast while far off, slowing as it closes, as a hand settles.
        const float ease = std::clamp(std::fabs(dy) / 25.0f, 0.35f, 1.0f);
        m.yaw = Wrap(m.yaw + std::clamp(dy, -most * ease, most * ease));
        m.pitch = std::clamp(m.pitch + std::clamp(dp, -most * ease, most * ease), -80.0f, 80.0f);
    }

    // The way toward `direction` that is open: the direction itself, or the
    // nearest to it of a fan either side, each a clear line at the hip and
    // the shoulder for the next stretch of floor. Turned round when all of them
    // are shut, which follows a wall to its end.
    // Once it has turned one way round something it keeps to that side, a
    // hand on the wall, until the way to the goal is open again and some time
    // has passed: choosing afresh every few ticks it went left, then right,
    // then left again, and stood at the first corner for ever.

    bool Open(const Player& me, const World& world, Vec3 step)
    {
        // Above a step and the rise of a slope, and at the shoulder; a low
        // wall under them is climbed (Unstick jumps).
        return world.visible(Up(me.origin, 40), Up(me.origin + step, 44), me.num) &&
               world.visible(Up(me.origin, 64), Up(me.origin + step, 66), me.num);
    }

    Vec3 Steer(const Player& me, const World& world, Vec3 direction, Detour& detour)
    {
        if (!world.visible) return direction;
        const float length = Flat(direction);
        if (length < 1) return direction;
        const float base = std::atan2(direction.y, direction.x);
        const float reach = std::min(length, 110.0f);
        auto stepAt = [&](float turn) {
            const float r = base + turn * kPi / 180.0f;
            return Vec3{ std::cos(r) * reach, std::sin(r) * reach, 0 };
        };
        if (Open(me, world, stepAt(0)))
        {
            if (world.time >= detour.until) detour.side = 0;
            if (!detour.side) return stepAt(0);
        }
        static const float kTurns[] = { 25, 50, 80, 115, 150 };
        if (detour.side)
        {
            for (float turn : kTurns)
                if (Open(me, world, stepAt(turn * float(detour.side)))) return stepAt(turn * float(detour.side));
            // That side is shut too: the other, from now.
            detour.side = -detour.side;
            detour.until = world.time + 2.5;
            return stepAt(90.0f * float(detour.side));
        }
        for (float turn : kTurns)
            for (int side : { 1, -1 })
                if (Open(me, world, stepAt(turn * float(side))))
                {
                    detour.side = side;
                    detour.until = world.time + 2.5;
                    return stepAt(turn * float(side));
                }
        return { -direction.x, -direction.y, 0 };
    }

    // Movement toward a world direction, as forward and right from the view.
    void MoveToward(Command& c, Vec3 direction, float yaw, float speed = 127)
    {
        const float length = Flat(direction);
        if (length < 1) return;
        const float r = yaw * kPi / 180.0f;
        const float x = direction.x / length, y = direction.y / length;
        const float forward = x * std::cos(r) + y * std::sin(r);
        const float right = x * std::sin(r) - y * std::cos(r);
        c.forward = int8_t(std::clamp(forward * speed, -127.0f, 127.0f));
        c.right = int8_t(std::clamp(right * speed, -127.0f, 127.0f));
    }

    // Where to go when there is nobody to fight: where it last saw somebody,
    // where the other team is, or somewhere else on the map.
    void ChooseGoal(Mind& m, const Player& me, const World& world)
    {
        m.hasGoal = false;
        m.path.clear();
        m.step = 0;
        if (m.hunting && world.time < m.huntUntil) { m.goal = m.lastSeen; m.hasGoal = true; }
        else
        {
            std::vector<const Player*> enemies;
            for (const Player& other : world.players) if (Enemy(me, other)) enemies.push_back(&other);
            if (!enemies.empty() && Random(m) < 0.75f)
            {
                m.goal = enemies[size_t(Random(m) * float(enemies.size())) % enemies.size()]->origin;
                m.hasGoal = true;
            }
            else if (!g_nodes.empty())
            {
                m.goal = g_nodes[size_t(Random(m) * float(g_nodes.size())) % g_nodes.size()].at;
                m.hasGoal = true;
            }
        }
        m.goalAt = world.time;
        if (m.hasGoal && !g_nodes.empty()) m.path = Route(Nearest(me.origin), Nearest(m.goal));
    }

    // The next point to walk to, along the waypoints or straight to the goal.
    bool NextPoint(Mind& m, const Player& me, Vec3& point, bool& crouch, bool& climb)
    {
        crouch = climb = false;
        if (!m.path.empty())
        {
            while (m.step < m.path.size())
            {
                const Node& node = g_nodes[size_t(m.path[m.step])];
                const Vec3 d = node.at - me.origin;
                if (Flat(d) > 40 || std::fabs(d.z) > 70) break;
                m.step++;
            }
            if (m.step < m.path.size())
            {
                const Node& node = g_nodes[size_t(m.path[m.step])];
                point = node.at;
                crouch = node.crouch;
                climb = node.climb;
                return true;
            }
            m.path.clear();
        }
        if (!m.hasGoal) return false;
        if (Flat(m.goal - me.origin) < 60) { m.hasGoal = false; return false; }
        point = m.goal;
        return true;
    }

    // Nearly nowhere in most of a second while trying to go somewhere: jump,
    // then step aside, then give the way up.
    void Unstick(Mind& m, const Player& me, const World& world, bool moving)
    {
        if (world.time - m.checkTime < 0.8) return;
        const float moved = Flat(me.origin - m.checkAt);
        m.checkAt = me.origin;
        m.checkTime = world.time;
        if (!moving || moved > 24) { m.stuck = 0; return; }
        m.stuck++;
        if (m.stuck == 1) m.jumpUntil = world.time + 0.25;
        else if (m.stuck == 2) { m.side = Random(m) < 0.5f ? -1 : 1; m.sideUntil = world.time + 0.7; }
        else
        {
            if (!m.path.empty() && m.step + 1 < m.path.size()) m.step++;
            else ChooseGoal(m, me, world);
            m.stuck = 0;
        }
    }
}

const char* bots::brain::Describe(uint32_t num)
{
    static char text[160];
    if (num >= 18) return "";
    const Mind& m = g_minds[num];
    std::snprintf(text, sizeof text, "target %d%s, goal %s (%.0f %.0f), path %zu/%zu, stuck %d, buttons %X",
                  m.target == UINT32_MAX ? -1 : int(m.target), m.hunting ? " hunting" : "",
                  m.hasGoal ? "yes" : "no", m.goal.x, m.goal.y, m.step, m.path.size(), m.stuck, m.lastButtons);
    return text;
}

void bots::brain::Dead(uint32_t num)
{
    if (num < 18) g_minds[num].alive = false;
}

void bots::brain::Observe(const World& world) { Learn(world); }

void bots::brain::NewMap(const char* map)
{
    SaveLearned();
    g_map = map ? map : "";
    g_savedNodes = 0;
    for (int& node : g_lastNode) node = -1;
    LoadWaypoints(g_map);
    g_savedNodes = g_nodes.size();
    for (Mind& m : g_minds) m = Mind{};
}

bots::brain::Command bots::brain::Think(uint32_t num, const World& world)
{
    Command c;
    if (num >= 18) return c;
    Mind& m = g_minds[num];
    const Player* self = Find(world, num);
    if (!self) return c;
    const Player& me = *self;
    if (!m.rng) m.rng = 0x9E3779B9u * (num + 1);
    const Params params = ParamsFor(world.skill);
    const float dt = std::clamp(float(world.time - m.last), 0.01f, 0.2f);
    m.last = world.time;
    if (!m.alive)
    {
        // A new life: the view it spawned with, and nothing remembered.
        const uint32_t rng = m.rng;
        m = Mind{};
        m.rng = rng;
        m.alive = true;
        m.yaw = me.view.y;
        m.pitch = 0;
        m.aimed = true;
        m.last = world.time;
        m.checkAt = me.origin;
        m.checkTime = world.time;
    }

    Look(m, me, world, params);
    const Player* target = m.target != UINT32_MAX ? Find(world, m.target) : nullptr;

    if (target)
    {
        // ---- a fight
        const Vec3 eye = Eye(me);
        const Vec3 aimAt = Up(target->origin, params.headHeight);
        const Vec3 to = aimAt - eye;
        const float distance = Length(to);
        // The error shrinks as it tracks, and is drawn again now and then.
        const double tracking = world.time - m.acquiredAt;
        const float settle = float(std::max(0.25, 1.0 - tracking / 1.4));
        if (world.time - m.errAt > 0.6)
        {
            const float spread = params.error * 0.6f;
            m.errYaw = (Random(m) * 2 - 1) * spread;
            m.errPitch = (Random(m) * 2 - 1) * spread * 0.6f;
            m.errAt = world.time;
        }
        Turn(m, PitchTo(to) + m.errPitch * settle, YawTo(to) + m.errYaw * settle, dt, params.turnRate);

        const float off = std::fabs(Wrap(YawTo(to) - m.yaw)) + std::fabs(PitchTo(to) - m.pitch);
        // On target: within the body's width at that distance, and a little.
        const float onTarget = std::clamp(std::atan2(22.0f, distance) * 180.0f / kPi, 1.0f, 12.0f) + 0.5f;
        const bool reacted = tracking >= params.reaction;
        if (distance < 70)
        {
            if (reacted) c.buttons |= kMelee;
        }
        else if (reacted && off < onTarget * 1.6f)
        {
            // Held, let go one tick in five: a semi-automatic fires again, an
            // automatic hardly notices.
            if (++m.fireTicks % 5 != 0) c.buttons |= kFire;
        }
        if (distance > 520 && tracking > 0.12) c.buttons |= kAds;

        // Move: strafe, close in from far, back off when too near.
        if (world.time >= m.strafeUntil)
        {
            m.strafe = Random(m) < 0.5f ? -1 : 1;
            m.strafeUntil = world.time + 0.35 + Random(m) * 0.9;
            if (world.skill >= Skill::Hardened && distance > 900 && Random(m) < 0.3f)
                m.crouchUntil = world.time + 0.8 + Random(m) * 1.2;
        }
        const Vec3 flatTo{ to.x, to.y, 0 };
        const float length = std::max(Flat(flatTo), 1.0f);
        const Vec3 toward{ flatTo.x / length, flatTo.y / length, 0 };
        const Vec3 aside{ toward.y * float(m.strafe), -toward.x * float(m.strafe), 0 };
        const float approach = distance > 800 ? 0.8f : distance < 220 ? -0.5f : 0.15f;
        MoveToward(c, { toward.x * approach + aside.x, toward.y * approach + aside.y, 0 }, m.yaw,
                   (c.buttons & kAds) ? 80.0f : 127.0f);
        if (world.time < m.crouchUntil) c.buttons |= kCrouch;
        if (distance < 300 && world.skill >= Skill::Regular && Random(m) < 0.01f) c.buttons |= kJump;
        m.hasGoal = false;
        m.path.clear();
    }
    else
    {
        // ---- on the move
        if (!m.hasGoal || world.time - m.goalAt > 25.0 ||
            (m.hunting && world.time >= m.huntUntil && Flat(m.goal - m.lastSeen) < 1))
        {
            if (m.hunting && world.time >= m.huntUntil) m.hunting = false;
            ChooseGoal(m, me, world);
        }
        Vec3 point;
        bool crouch = false, climb = false;
        const bool going = NextPoint(m, me, point, crouch, climb);
        if (going)
        {
            const Vec3 d = point - me.origin;
            // Looking where it goes, a little ahead along the path, eyes level.
            Vec3 lookAt = point;
            if (!m.path.empty() && m.step + 1 < m.path.size()) lookAt = g_nodes[size_t(m.path[m.step + 1])].at;
            if (m.hunting) lookAt = m.lastSeen;
            // Off a path the view turns with the way the feelers found.
            if (m.path.empty() && Flat(m.steer) > 1) lookAt = me.origin + m.steer;
            const Vec3 look = Up(lookAt, 50) - Eye(me);
            Turn(m, std::clamp(PitchTo(look), -15.0f, 15.0f), YawTo(look), dt, params.turnRate * 0.6f);
            if (m.path.empty() || m.stuck > 0)
            {
                if (++m.steerTicks % 3 == 0 || m.steerTicks == 1) m.steer = Steer(me, world, d, m.detour);
                MoveToward(c, m.steer, m.yaw);
            }
            else
                MoveToward(c, d, m.yaw);
            if (crouch) c.buttons |= kCrouch;
            if (climb) c.buttons |= kJump;
            // Sprint across the open, not while hunting somebody just lost.
            if (!m.hunting && Flat(d) > 250 && std::fabs(Wrap(YawTo(d) - m.yaw)) < 30) c.buttons |= kSprint;
        }
        else
        {
            // Nowhere to go yet: look round.
            Turn(m, 0, m.yaw + 40, dt, 60);
        }
        if (world.time < m.jumpUntil) c.buttons |= kJump;
        if (world.time < m.sideUntil)
        {
            const float r = m.yaw * kPi / 180.0f;
            MoveToward(c, { std::sin(r) * float(m.side), -std::cos(r) * float(m.side), 0 }, m.yaw);
        }
        Unstick(m, me, world, going);
        // Reload in a quiet moment.
        if (world.time - m.seenAt > 2.0 && world.time - m.lastReload > 14.0)
        {
            c.buttons |= kReload;
            m.lastReload = world.time;
        }
    }

    c.pitch = m.pitch;
    c.yaw = m.yaw;
    m.lastButtons = c.buttons;
    return c;
}
