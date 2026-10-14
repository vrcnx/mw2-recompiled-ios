#pragma once
// What the bots read and call in the multiplayer's engine: IW4's server and
// game structures as the disc's multiplayer lays them out, and the functions
// that add a test client, run a client's command and trace a line of sight.
//
// Found in default_mp.xex (disc version) by what uses them:
//   - SV_UpdateBots (sub_822C2BD8) walks svs.clients every server frame and
//     calls SV_BotUserMove (sub_822C29B8) for each test client; the latter
//     builds the client's usercmd -- serverTime, buttons at +4 (fire 0x1,
//     use-reload 0x20, crouch 0x200: the testClients_do* dvars), angles at
//     +8, weapon at +20, forward and right moves at +26 and +27 -- and runs it
//     with SV_ClientThink (sub_822BBF30).
//   - addtestclient (sub_82250890) is SV_AddTestClient (sub_822BD470), which
//     returns the new client's gentity.
//   - The script field tables give the gentity's origin (0x138), angles
//     (0x144) and health (0x19C), and the gclient's score, deaths and kills;
//     the sessionteam and sessionstate getters read the team (0x3250) and the
//     session state (0x3180). getplayerangles reads ps.viewangles (+268),
//     isonground ps.groundEntityNum (+108); SetClientViewAngle (sub_8221ABE8)
//     makes ps.delta_angles (+0x60, floats) the angle less the last command's
//     (sess.cmd.angles, +12728).
//   - sighttracepassed calls G_SightTrace (sub_82233220): hit out, start,
//     end, the entity to pass, the contents to stop at.
//   - Cmd_MenuResponse_f (sub_82224818) pushes the response and the menu name
//     with Scr_AddString (sub_822AD7A8) and notifies "menuresponse"
//     (scr_const, +480) with Scr_Notify (sub_82259568): how a player chooses a
//     team and a class, and how a bot is made to.
//   - SV_SpawnServer (sub_822C0278) is given the map's name.
#include <cstdint>

#if defined(MW2_TITLE_MP) && defined(MW2_VERSION_TU0)
#define MW2_BOTS_KNOWN 1

#define T_SV_UpdateBots      822C2BD8
#define T_SV_BotUserMove     822C29B8
#define T_SV_AddTestClient   822BD470
#define T_SV_ClientThink     822BBF30
#define T_SV_SpawnServer     822C0278
#define T_G_SightTrace       82233220
#define T_Scr_AddString      822AD7A8
#define T_Scr_Notify         82259568

namespace bots::game
{
    // svs
    constexpr uint32_t kSvs = 0x83620400u;
    constexpr uint32_t kSvsTime = 14352, kSvsClientCount = 14360, kSvsClients = 14364;
    constexpr uint32_t kClientStride = 0x97F80u;
    // client_t
    constexpr uint32_t kClState = 0, kClDeltaMessage = 8, kClOutgoingSequence = 24, kClAddressType = 40;
    constexpr uint32_t kClGentity = 0x21294u;
    constexpr uint32_t kAddressBot = 0;      // netadr type of a test client
    // gentity_s
    constexpr uint32_t kEntities = 0x82DD4300u, kEntityStride = 640;
    constexpr uint32_t kEntOrigin = 0x138, kEntAngles = 0x144, kEntClient = 344, kEntHealth = 0x19C;
    // gclient_s, which begins with its playerState_s
    constexpr uint32_t kPsGroundEntity = 108, kPsDeltaAngles = 0x60, kPsViewAngles = 268, kPsWeapon = 692;
    constexpr uint32_t kClCmdAngles = 12728;
    constexpr uint32_t kClSessionState = 0x3180, kClTeam = 0x3250;
    constexpr uint32_t kClScore = 0x3198, kClDeaths = 0x319C, kClKills = 0x31A0;
    constexpr uint32_t kGroundNone = 2047;
    // scr_const
    constexpr uint32_t kScrConst = 0x82FFA880u, kConstMenuResponse = 480;

    enum Team : uint32_t { kTeamFree = 0, kTeamAxis = 1, kTeamAllies = 2, kTeamSpectator = 3 };
    enum Session : uint32_t { kPlaying = 0, kDead = 1, kSpectator = 2, kIntermission = 3 };

    // usercmd_s buttons, as IW4's are.
    constexpr uint32_t kFire = 0x1, kSprint = 0x2, kMelee = 0x4, kUse = 0x8, kReload = 0x10,
                       kUseReload = 0x20, kProne = 0x100, kCrouch = 0x200, kJump = 0x400,
                       kAds = 0x800, kFrag = 0x4000;
    constexpr uint32_t kUsercmdSize = 40;

    // The contents a line of sight stops at: the world without the players
    // (sighttracepassed's when told not to hit characters).
    constexpr uint32_t kSightMask = 0x00801803u;
}
#endif
