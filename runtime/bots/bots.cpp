// The engine side of the bots: adding them, choosing their team and class
// the way a player does, and running each one's usercmd from the brain.
// Everything here runs on the server's own thread, inside SV_UpdateBots and
// SV_BotUserMove (bots/engine.h).
#include <ppc_recomp_shared.h>
#include "bots.h"
#include "engine.h"
#include "brain.h"
#include "../title.h"
#include "../guest.h"
#include "../env.h"
#include "../log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

namespace
{
    std::atomic<uint32_t> g_count{ UINT32_MAX };   // unset: MW2_BOTS on first use
    std::atomic<int> g_skill{ -1 };
    std::atomic<int> g_teams{ -1 };
}

void bots::SetCount(uint32_t count) { g_count = std::min(count, 17u); }
void bots::SetSkill(Skill skill) { g_skill = int(skill); }
void bots::SetTeams(Teams teams) { g_teams = int(teams); }

uint32_t bots::Count()
{
    uint32_t count = g_count.load();
    if (count == UINT32_MAX)
    {
        count = std::min<uint32_t>(uint32_t(env::Number("MW2_BOTS", 0)), 17u);
        g_count = count;
    }
    return count;
}

#ifdef MW2_BOTS_KNOWN
using namespace bots::game;

namespace
{
    using Clock = std::chrono::steady_clock;

    uint32_t R32(uint32_t at) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(guest::Base() + at)); }
    float RF(uint32_t at) { const uint32_t v = R32(at); float f; std::memcpy(&f, &v, 4); return f; }
    void W32(uint32_t at, uint32_t v) { *reinterpret_cast<uint32_t*>(guest::Base() + at) = __builtin_bswap32(v); }
    void W16(uint32_t at, uint16_t v) { *reinterpret_cast<uint16_t*>(guest::Base() + at) = __builtin_bswap16(v); }
    void W8(uint32_t at, uint8_t v) { guest::Base()[at] = v; }
    void WF(uint32_t at, float f) { uint32_t v; std::memcpy(&v, &f, 4); W32(at, v); }
    uint16_t R16(uint32_t at) { return __builtin_bswap16(*reinterpret_cast<const uint16_t*>(guest::Base() + at)); }

    // A call into the title from a hook: its own registers kept, the callee's
    // frame below the hook's, and a scratch area between for what is passed
    // by address.
    struct Calls
    {
        PPCContext& ctx;
        uint8_t* base;
        uint32_t scratch, used = 0;

        Calls(PPCContext& c, uint8_t* b) : ctx(c), base(b), scratch((uint32_t(c.r1.u32) - 0x700) & ~0xFu) {}

        uint32_t Put(const void* data, uint32_t bytes)
        {
            const uint32_t at = scratch + used;
            std::memcpy(base + at, data, bytes);
            used += (bytes + 15) & ~15u;
            return at;
        }
        uint32_t String(const char* text) { return Put(text, uint32_t(std::strlen(text) + 1)); }
        uint32_t Vector(const bots::brain::Vec3& v)
        {
            const uint32_t at = scratch + used;
            WF(at, v.x); WF(at + 4, v.y); WF(at + 8, v.z);
            used += 16;
            return at;
        }

        uint64_t Call(PPCFunc* fn, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0, uint64_t e = 0)
        {
            const PPCContext saved = ctx;
            ctx.r1.u64 = (uint64_t(scratch) - 0x200) & ~0xFull;
            ctx.r3.u64 = a; ctx.r4.u64 = b; ctx.r5.u64 = c; ctx.r6.u64 = d; ctx.r7.u64 = e;
            fn(ctx, base);
            const uint64_t result = ctx.r3.u64;
            ctx = saved;
            used = 0;
            return result;
        }
    };

    uint32_t ClientAt(uint32_t index) { return R32(kSvs + kSvsClients) + index * kClientStride; }
    uint32_t ClientCount() { return R32(kSvs + kSvsClientCount); }
    bool IsBot(uint32_t client) { return R32(client + kClState) != 0 && R32(client + kClAddressType) == kAddressBot; }
    bool IsHuman(uint32_t client) { return R32(client + kClState) != 0 && R32(client + kClAddressType) != kAddressBot; }
    uint32_t Gentity(uint32_t client) { return R32(client + kClGentity); }

    // What the server knows of each bot that the brain does not.
    struct Joined
    {
        Clock::time_point connected;
        int step = 0;            // 0 nothing sent, 1 team chosen, 2 class chosen
        Clock::time_point sent;
    };
    Joined g_joined[18];
    Clock::time_point g_lastAdded;
    std::string g_map;
    bool g_told = false;

    bots::brain::Skill SkillNow()
    {
        int skill = g_skill.load();
        if (skill < 0)
        {
            const char* text = env::Text("MW2_BOT_SKILL");
            skill = text ? std::clamp(std::atoi(text), 0, 3) : 1;
            g_skill = skill;
        }
        return bots::brain::Skill(skill);
    }

    bool BothTeams()
    {
        int teams = g_teams.load();
        if (teams < 0)
        {
            const char* text = env::Text("MW2_BOT_TEAMS");
            teams = text && std::strcmp(text, "against") == 0 ? int(bots::Teams::Against) : int(bots::Teams::Both);
            g_teams = teams;
        }
        return teams == int(bots::Teams::Both);
    }

    // The menu a player answers, the way Cmd_MenuResponse_f passes it on.
    void MenuResponse(Calls& calls, uint32_t entity, const char* menu, const char* response)
    {
        const uint32_t responseText = calls.String(response);
        calls.Call(GUEST_FUNC(T_Scr_AddString), responseText);
        const uint32_t menuText = calls.String(menu);
        calls.Call(GUEST_FUNC(T_Scr_AddString), menuText);
        calls.Call(GUEST_FUNC(T_Scr_Notify), entity, R16(kScrConst + kConstMenuResponse), 2);
    }

    bots::brain::Vec3 Vec(uint32_t at) { return { RF(at), RF(at + 4), RF(at + 8) }; }

    // Every player the server has, as the brain sees them.
    void Gather(bots::brain::World& world)
    {
        world.players.clear();
        const uint32_t count = std::min(ClientCount(), 18u);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t client = ClientAt(i);
            if (!R32(client + kClState)) continue;
            const uint32_t entity = kEntities + i * kEntityStride;
            const uint32_t gclient = R32(entity + kEntClient);
            if (!gclient) continue;
            bots::brain::Player p;
            p.num = i;
            p.bot = IsBot(client);
            p.team = R32(gclient + kClTeam);
            p.health = int32_t(R32(entity + kEntHealth));
            p.alive = R32(gclient + kClSessionState) == kPlaying && p.health > 0;
            p.origin = Vec(entity + kEntOrigin);
            p.view = Vec(gclient + kPsViewAngles);
            p.onGround = R32(gclient + kPsGroundEntity) != kGroundNone;
            world.players.push_back(p);
        }
    }

    // How many bots to add or take away, and the joining of new ones.
    void Manage(PPCContext& ctx, uint8_t* base)
    {
        const uint32_t count = std::min(ClientCount(), 18u);
        if (!count) return;
        uint32_t bots = 0, humans = 0;
        int lastBot = -1;
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t client = ClientAt(i);
            if (IsBot(client)) { bots++; lastBot = int(i); }
            else if (IsHuman(client)) humans++;
            else g_joined[i] = Joined{};
        }
        // Only on a server with somebody to play: this machine hosting a match.
        if (!humans) return;
        const auto now = Clock::now();
        Calls calls(ctx, base);
        const uint32_t wanted = bots::Count();
        if (bots < wanted && humans + bots < count && now - g_lastAdded > std::chrono::milliseconds(600))
        {
            g_lastAdded = now;
            const uint32_t entity = uint32_t(calls.Call(GUEST_FUNC(T_SV_AddTestClient)));
            if (entity)
            {
                const uint32_t num = (entity - kEntities) / kEntityStride;
                if (num < 18) g_joined[num] = Joined{ now, 0, now };
                LOGI("bots: added a bot as client %u (%u of %u)", num, bots + 1, wanted);
            }
            else if (!g_told)
            {
                g_told = true;
                LOGW("bots: the server would not add a test client");
            }
        }
        else if (bots > wanted && lastBot >= 0 && now - g_lastAdded > std::chrono::milliseconds(300))
        {
            g_lastAdded = now;
            char line[32];
            std::snprintf(line, sizeof line, "clientkick %d\n", lastBot);
            calls.Call(GUEST_FUNC(T_Cbuf_AddText), 0, calls.String(line));
            LOGI("bots: dropped the bot at client %d", lastBot);
        }

        // A new bot answers the team menu, then the class one, as a player
        // would with a moment between.
        uint32_t hostTeam = kTeamFree;
        for (uint32_t i = 0; i < count; i++)
            if (IsHuman(ClientAt(i)))
            {
                const uint32_t gclient = R32(kEntities + i * kEntityStride + kEntClient);
                if (gclient) { hostTeam = R32(gclient + kClTeam); break; }
            }
        uint32_t onTeam[4] = {};
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t gclient = R32(kEntities + i * kEntityStride + kEntClient);
            if (gclient && R32(ClientAt(i) + kClState)) onTeam[std::min(R32(gclient + kClTeam), 3u)]++;
        }
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t client = ClientAt(i);
            if (!IsBot(client)) continue;
            Joined& joined = g_joined[i];
            const uint32_t entity = kEntities + i * kEntityStride;
            const uint32_t gclient = R32(entity + kEntClient);
            if (!gclient) continue;
            const uint32_t team = R32(gclient + kClTeam);
            const uint32_t session = R32(gclient + kClSessionState);
            // The connect script puts a new player with the spectators before
            // it listens for his menus: a response sent sooner is lost. Each
            // step is sent again until it takes.
            const bool resend = now - joined.sent > std::chrono::seconds(3);
            if (team == kTeamSpectator && (joined.step == 0 || resend) &&
                now - joined.connected > std::chrono::milliseconds(700))
            {
                // Against the host: the other team, where there are teams.
                const char* choice = "autoassign";
                if (!BothTeams() && (hostTeam == kTeamAxis || hostTeam == kTeamAllies))
                    choice = hostTeam == kTeamAxis ? "allies" : "axis";
                MenuResponse(calls, entity, "team_marinesopfor", choice);
                joined.step = 1;
                joined.sent = now;
                LOGI("bots: client %u chose %s", i, choice);
            }
            else if (team != kTeamSpectator && session != kPlaying && session != kDead &&
                     (joined.step < 2 || resend) && now - joined.sent > std::chrono::milliseconds(600))
            {
                // One of the default classes, so every bot plays with what
                // the game gives, whatever this profile has unlocked.
                char response[16];
                std::snprintf(response, sizeof response, "class%u", unsigned(i % 5));
                MenuResponse(calls, entity, "changeclass", response);
                joined.step = 2;
                joined.sent = now;
                LOGI("bots: client %u chose %s", i, response);
            }
        }
    }

    bots::brain::World g_world;
}

// MW2_BOT_LOG=1: every two seconds, each player as the bots see them.
void LogPlayers()
{
    static const bool on = env::Flag("MW2_BOT_LOG");
    static Clock::time_point last;
    if (!on || Clock::now() - last < std::chrono::seconds(2)) return;
    last = Clock::now();
    for (const bots::brain::Player& p : g_world.players)
    {
        const uint32_t gclient = R32(kEntities + p.num * kEntityStride + kEntClient);
        LOGI("bots: %s%u team %u %s hp %d at %.0f %.0f %.0f yaw %.0f, %d kills %d deaths; %s",
             p.bot ? "bot " : "player ", p.num, p.team, p.alive ? "alive" : "down", p.health,
             p.origin.x, p.origin.y, p.origin.z, p.view.y,
             gclient ? int32_t(R32(gclient + kClKills)) : 0, gclient ? int32_t(R32(gclient + kClDeaths)) : 0,
             p.bot ? bots::brain::Describe(p.num) : "");
    }
}

GUEST_HOOK(T_SV_UpdateBots)
{
    Manage(ctx, base);
    Gather(g_world);
    LogPlayers();
    g_world.time = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    g_world.skill = SkillNow();
    bots::brain::Observe(g_world);
    GUEST_ORIG(T_SV_UpdateBots)(ctx, base);
}

// A bot's usercmd: the brain's, where the stock one only wanders. A dead bot,
// or one in a killcam, is left to the stock code, which skips the killcam.
GUEST_HOOK(T_SV_BotUserMove)
{
    const uint32_t client = ctx.r3.u32;
    const uint32_t entity = Gentity(client);
    const uint32_t num = entity ? (entity - kEntities) / kEntityStride : 18;
    const uint32_t gclient = entity ? R32(entity + kEntClient) : 0;
    if (!entity || num >= 18 || !gclient || R32(gclient + kClSessionState) != kPlaying ||
        int32_t(R32(entity + kEntHealth)) <= 0)
    {
        bots::brain::Dead(num);
        GUEST_ORIG(T_SV_BotUserMove)(ctx, base);
        return;
    }

    Calls calls(ctx, base);
    g_world.visible = [&calls](const bots::brain::Vec3& from, const bots::brain::Vec3& to, uint32_t pass) {
        const uint32_t hit = calls.scratch + 0x500;
        W32(hit, 1);
        const uint32_t start = calls.Vector(from), end = calls.Vector(to);
        calls.Call(GUEST_FUNC(T_G_SightTrace), hit, start, end, pass, kSightMask);
        return R32(hit) == 0;
    };
    // MW2_BOT_LOG: now and then, what the line of sight answers for a bot.
    static const bool probe = env::Flag("MW2_BOT_LOG");
    static Clock::time_point probed;
    if (probe && Clock::now() - probed > std::chrono::seconds(3))
    {
        probed = Clock::now();
        const bots::brain::Vec3 at = Vec(entity + kEntOrigin);
        const bots::brain::Vec3 eye{ at.x, at.y, at.z + 60 };
        const bool up = g_world.visible(eye, { at.x, at.y, at.z + 200 }, num);
        const bool ahead = g_world.visible(eye, { at.x + 40, at.y, at.z + 60 }, num);
        const bool down = g_world.visible(eye, { at.x, at.y, at.z - 200 }, num);
        LOGI("bots: probe bot %u: up %d, a step ahead %d, through the floor %d; ground %u",
             num, up, ahead, down, R32(gclient + kPsGroundEntity));
    }
    bots::brain::Command command = bots::brain::Think(num, g_world);
    g_world.visible = nullptr;
    // MW2_BOT_TEST=spin: turn at 90 degrees a second and walk forward, to see
    // the view and the walk take.
    static const char* test = env::Text("MW2_BOT_TEST");
    if (test && std::strcmp(test, "spin") == 0)
    {
        command = {};
        command.yaw = float(std::fmod(g_world.time * 90.0, 360.0)) - 180.0f;
        command.forward = 127;
    }

    // The usercmd as SV_BotUserMove makes it, on the guest's stack.
    const uint32_t cmd = calls.scratch + 0x400;
    std::memset(base + cmd, 0, kUsercmdSize);
    W32(cmd + 0, R32(kSvs + kSvsTime) - 200);
    W32(cmd + 4, command.buttons);
    // The view the engine gives is the command's angles plus delta_angles.
    const float pitch = command.pitch - RF(gclient + kPsDeltaAngles);
    const float yaw = command.yaw - RF(gclient + kPsDeltaAngles + 4);
    W32(cmd + 8, uint32_t(int32_t(std::lround(pitch * 65536.0 / 360.0)) & 0xFFFF));
    W32(cmd + 12, uint32_t(int32_t(std::lround(yaw * 65536.0 / 360.0)) & 0xFFFF));
    W16(cmd + 20, uint16_t(R32(gclient + kPsWeapon)));
    W8(cmd + 26, uint8_t(command.forward));
    W8(cmd + 27, uint8_t(command.right));
    W32(client + kClDeltaMessage, R32(client + kClOutgoingSequence) - 1);
    calls.Call(GUEST_FUNC(T_SV_ClientThink), client, cmd);
}

GUEST_HOOK(T_SV_SpawnServer)
{
    if (const char* name = reinterpret_cast<const char*>(base + ctx.r3.u32); ctx.r3.u32 && name[0])
    {
        g_map.assign(name, strnlen(name, 64));
        LOGI("bots: the server is starting %s", g_map.c_str());
        bots::brain::NewMap(g_map.c_str());
        for (Joined& joined : g_joined) joined = Joined{};
    }
    GUEST_ORIG(T_SV_SpawnServer)(ctx, base);
}
#endif

#if defined(MW2_BOTS_KNOWN) && MW2_DIAGNOSTICS
// MW2_BOT_LOG=1 also names every menu response a player makes, as the scripts
// are given it: the menus' names and answers, which the bots give in turn.
namespace { bool g_inMenuResponse = false; }
#define T_Cmd_MenuResponse_f 82224818
GUEST_HOOK(T_Cmd_MenuResponse_f)
{
    g_inMenuResponse = env::Flag("MW2_BOT_LOG");
    GUEST_ORIG(T_Cmd_MenuResponse_f)(ctx, base);
    g_inMenuResponse = false;
}
GUEST_HOOK(T_Scr_AddString)
{
    if (g_inMenuResponse && ctx.r3.u32)
        LOGI("bots: a menu response pushes \"%.64s\"", reinterpret_cast<const char*>(base + ctx.r3.u32));
    GUEST_ORIG(T_Scr_AddString)(ctx, base);
}
#endif

#if defined(MW2_BOTS_KNOWN) && MW2_DIAGNOSTICS
// MW2_DUMP_SCRIPTS=<dir>: every script's source as the title loads it
// (Scr_AddSourceBuffer, sub_8229EA80, from Scr_LoadScript), under its name.
#include <filesystem>
#include <fstream>
#define T_Scr_AddSourceBuffer 8229EA80
GUEST_HOOK(T_Scr_AddSourceBuffer)
{
    const uint32_t nameAt = ctx.r4.u32;
    std::string name = nameAt ? std::string(reinterpret_cast<const char*>(base + nameAt), strnlen(reinterpret_cast<const char*>(base + nameAt), 128)) : "";
    GUEST_ORIG(T_Scr_AddSourceBuffer)(ctx, base);
    static const char* folder = env::Text("MW2_DUMP_SCRIPTS");
    const uint32_t text = ctx.r3.u32;
    if (!folder || !text || name.empty()) return;
    const std::filesystem::path path = std::filesystem::path(folder) / name;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const char* source = reinterpret_cast<const char*>(base + text);
    std::ofstream(path, std::ios::binary).write(source, std::streamsize(strnlen(source, 4u << 20)));
}
#endif
