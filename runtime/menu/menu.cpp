#include "menu.h"
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include "../env.h"
#include "../log.h"
#include "../settings.h"
#include "../crash.h"
#include "../field_of_view.h"
#include "profile.h"   // launcher/profile.h: the saved stats' editor

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>

namespace
{
    using Clock = std::chrono::steady_clock;

    // XINPUT_GAMEPAD's buttons.
    constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008;
    constexpr uint16_t kStart = 0x0010, kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000;
    constexpr uint16_t kChord = kRB | kStart;

    struct Choice { std::string label, value; };

    // One entry: what it is called, what it does, and the switch it writes.
    struct Setting
    {
        int category;
        std::string label, description;
        std::vector<Choice> choices;        // empty for an action
        bool restart = false;
        // Reads the current choice from the environment, and writes one.
        std::function<int()> current;
        std::function<void(const std::string&)> store;
        std::function<void()> act;          // an action's work
        int chosen = 0;
        bool header = false;
    };

    struct Category { std::string name, description; };
    const std::vector<Category> kCategories = {
        { "Display", "How the picture reaches the screen." },
        { "Graphics", "What the renderer draws." },
        { "Gameplay", "How the game looks in play." },
        { "Cheats", "Rank and unlocks, written to your saved stats the next time the game starts." },
        { "System", "The menu itself, and quitting to apply what waits for the next start." },
    };

    struct State
    {
        std::mutex lock;
        bool built = false, open = false, chordHeld = false;
        std::vector<Setting> settings;
        int category = 0, row = 0;     // row: an index into the category's entries
        std::string notice;
        Clock::time_point noticeAt;
        bool noticeSeen = false;
        bool restartPending = false;
        // Navigation: what was held at the last read, and when a held
        // direction next repeats.
        uint16_t held = 0;
        Clock::time_point repeatAt;
        bool stickUp = false, stickDown = false, stickLeft = false, stickRight = false;
        std::filesystem::path file = ".env";
    } s;

    std::atomic<bool> g_open{ false };
    std::atomic<bool> g_counter{ false };

    void Notice(const std::string& text)
    {
        s.notice = text;
        s.noticeAt = Clock::now();
        s.noticeSeen = s.open;
    }

    void Store(const std::string& name, const std::string& value)
    {
        if (!settings::Store(s.file, name, value))
            LOGW("menu: could not write %s to %s", name.c_str(), s.file.string().c_str());
    }

    // A switch with one value per choice; "" is unset.
    Setting Switch(int category, std::string label, std::string description, const char* name,
                   std::vector<Choice> choices, bool restart)
    {
        Setting setting;
        setting.category = category;
        setting.label = std::move(label);
        setting.description = std::move(description);
        setting.choices = std::move(choices);
        setting.restart = restart;
        const std::string variable = name;
        const std::vector<Choice> copy = setting.choices;
        setting.current = [variable, copy] {
            const char* value = env::Text(variable.c_str());
            const std::string text = value ? value : "";
            for (size_t i = 0; i < copy.size(); i++)
                if (copy[i].value == text) return int(i);
            return 0;
        };
        setting.store = [variable](const std::string& value) { Store(variable, value); };
        return setting;
    }

    Setting Header(int category, std::string label)
    {
        Setting setting;
        setting.category = category;
        setting.label = std::move(label);
        setting.header = true;
        return setting;
    }

    Setting Action(int category, std::string label, std::string description, std::function<void()> act)
    {
        Setting setting;
        setting.category = category;
        setting.label = std::move(label);
        setting.description = std::move(description);
        setting.act = std::move(act);
        return setting;
    }

    void Build()
    {
        if (s.built) return;
        s.built = true;
        auto& list = s.settings;

        // ---- Display
        list.push_back(Switch(0, "Aspect Ratio",
            "4:3 fills an iPad's screen: the game lays out its view and HUD for a 4:3 television, as it "
            "did on one. 16:9 is the widescreen picture, with bars above and below it.",
            "MW2_ASPECT", {
#if defined(__APPLE__) && TARGET_OS_IOS
                { "4:3 (fill)", "" }, { "16:9 (letterbox)", "16:9" },
#else
                { "16:9", "" }, { "4:3 (fill)", "4:3" },
#endif
            }, true));
        list.push_back(Switch(0, "Resolution",
            "How many times the console's 1280x720 the game is drawn at, before it is fitted to the screen. "
            "Sharper at 2x; 3x asks a lot of the graphics chip.",
            "MW2_SCALE", { { "1x (720p)", "" }, { "2x (1440p)", "2" }, { "3x (2160p)", "3" } }, true));
        list.push_back(Switch(0, "Frame Rate",
            "60 is the console's pacing, a frame at every blank. Higher takes the game off the blank to "
            "its own limiter: smoother on a 120 Hz screen, and new to the game, so experimental.",
            "MW2_FPS_LIMIT", { { "60 (console)", "" }, { "90", "90" }, { "120", "120" } }, true));
        {
            Setting counter = Switch(0, "Frame Counter",
                "Frames a second in the corner of the screen.",
                "MW2_FPS_COUNTER", { { "Off", "" }, { "On", "1" } }, false);
            const auto store = counter.store;
            counter.store = [store](const std::string& value) { store(value); g_counter = !value.empty(); };
            list.push_back(std::move(counter));
        }

        // ---- Graphics
        {
            Setting msaa;
            msaa.category = 1;
            msaa.label = "Anti-Aliasing";
            msaa.description = "The console draws the world with 2x multisampling. 4x smooths edges further; "
                               "off is the lightest on the graphics chip.";
            msaa.choices = { { "Console (2x)", "" }, { "4x", "4" }, { "Off", "off" } };
            msaa.restart = true;
            msaa.current = [] {
                if (env::Flag("MW2_NO_MSAA")) return 2;
                return env::Number("MW2_MSAA") >= 4 ? 1 : 0;
            };
            msaa.store = [](const std::string& value) {
                Store("MW2_NO_MSAA", value == "off" ? "1" : "");
                Store("MW2_MSAA", value == "4" ? "4" : "");
            };
            list.push_back(std::move(msaa));
        }

        // ---- Gameplay
        {
            std::vector<Choice> angles;
            for (int degrees = 65; degrees <= 120; degrees += 5)
                angles.push_back({ std::to_string(degrees) + (degrees == 65 ? " (console)" : ""),
                                   degrees == 65 ? "" : std::to_string(degrees) });
            Setting fov = Switch(2, "Field of View",
                "How wide the view is, counted as the game's own cg_fov: 65 on the console. Aiming down "
                "the sights, killcams and turrets widen with it.",
                "MW2_FOV", angles, false);
            const auto store = fov.store;
            fov.store = [store](const std::string& value) {
                store(value);
                fov::Set(value.empty() ? 65.0 : std::atof(value.c_str()));
            };
            list.push_back(std::move(fov));
        }

        // ---- Cheats
        list.push_back(Header(3, "Multiplayer"));
        list.push_back(Switch(3, "Rank",
            "Level 70, the top of the current prestige, at the next start.",
            "MW2_CHEAT_MAX_RANK", { { "As earned", "" }, { "Max (level 70)", "1" } }, true));
        {
            std::vector<Choice> prestiges = { { "As earned", "" } };
            for (int p = 0; p <= profile::MaxPrestige(); p++)
                prestiges.push_back({ p ? "Prestige " + std::to_string(p) : "None", std::to_string(p) });
            list.push_back(Switch(3, "Prestige",
                "Sets the prestige the game starts with, from none to the tenth.",
                "MW2_CHEAT_PRESTIGE", prestiges, true));
        }
        list.push_back(Switch(3, "Unlock Everything",
            "Every challenge done, and every title, emblem and killstreak unlocked. Weapons and their "
            "attachments and camouflage come with the challenges.",
            "MW2_CHEAT_UNLOCK_ALL", { { "Off", "" }, { "On", "1" } }, true));

        // ---- System
        list.push_back(Action(4, "Apply and Quit",
            "Saves everything and closes the game, so what waits for the next start takes effect. "
            "Open it again from the home screen.",
            [] { crash::RequestExit("quit from the settings menu"); }));
        list.push_back(Action(4, "Restore Defaults",
            "Every setting here back to the console's, from the next start.",
            [] {
                for (Setting& setting : s.settings)
                    if (!setting.header && !setting.choices.empty() && setting.category != 3)
                    {
                        setting.chosen = 0;
                        setting.store(setting.choices[0].value);
                        if (setting.restart) s.restartPending = true;
                    }
                Notice("Settings restored to the console's");
            }));
        list.push_back(Action(4, "Menu Shortcut: RB + Start",
            "Hold RB and press Start to open or close this menu. B closes it too.", [] {}));

        for (Setting& setting : list)
            if (setting.current) setting.chosen = setting.current();
        g_counter = env::Flag("MW2_FPS_COUNTER");
    }

    std::vector<int> Entries(int category)
    {
        std::vector<int> out;
        for (int i = 0; i < int(s.settings.size()); i++)
            if (s.settings[i].category == category) out.push_back(i);
        return out;
    }

    // The first entry at or after `row` (in `step`'s direction) that can be chosen.
    int Selectable(const std::vector<int>& entries, int row, int step)
    {
        const int count = int(entries.size());
        for (int tries = 0; tries < count; tries++, row += step)
        {
            row = (row % count + count) % count;
            if (!s.settings[entries[row]].header) return row;
        }
        return 0;
    }

    void Step(Setting& setting, int by)
    {
        if (setting.choices.empty()) return;
        const int count = int(setting.choices.size());
        const int next = std::clamp(setting.chosen + by, 0, count - 1);
        if (next == setting.chosen) return;
        setting.chosen = next;
        setting.store(setting.choices[next].value);
        if (setting.restart) s.restartPending = true;
    }

    void Activate(Setting& setting)
    {
        if (setting.act) { setting.act(); return; }
        // A on a value steps it on, round to the first after the last.
        if (setting.choices.empty()) return;
        const int count = int(setting.choices.size());
        setting.chosen = (setting.chosen + 1) % count;
        setting.store(setting.choices[setting.chosen].value);
        if (setting.restart) s.restartPending = true;
    }

    // The left stick as four buttons, with a gap between where a push counts
    // and where letting go does: a stick resting near the threshold used to
    // flicker across it and step two or three rows at once.
    void Stick(const int16_t axes[4], uint16_t& directions)
    {
        constexpr int kPress = 20000, kRelease = 12000;
        auto edge = [](bool& on, int value) {
            if (!on && value > kPress) on = true;
            else if (on && value < kRelease) on = false;
            return on;
        };
        // The guest's Y axis points up.
        if (edge(s.stickUp, axes[1])) directions |= kUp;
        if (edge(s.stickDown, -int(axes[1]))) directions |= kDown;
        if (edge(s.stickRight, axes[0])) directions |= kRight;
        if (edge(s.stickLeft, -int(axes[0]))) directions |= kLeft;
    }
}

void menu::SetSettingsFile(const std::filesystem::path& path)
{
    std::lock_guard held(s.lock);
    s.file = path;
}

bool menu::Open() { return g_open.load(std::memory_order_relaxed); }
bool menu::FrameCounter() { return g_counter.load(std::memory_order_relaxed); }

bool menu::Input(uint16_t buttons, const int16_t axes[4])
{
    std::lock_guard held(s.lock);
    const bool chord = (buttons & kChord) == kChord;
    if (chord && !s.chordHeld)
    {
        Build();
        s.open = !s.open;
        g_open = s.open;
        s.held = buttons;   // nothing held now counts as a fresh press
        // A notice left from the start (the cheats) is shown from now.
        if (s.open && !s.notice.empty() && !s.noticeSeen) { s.noticeAt = Clock::now(); s.noticeSeen = true; }
        LOGI("menu: %s", s.open ? "opened" : "closed");
    }
    s.chordHeld = chord;
    // The chord itself is not the title's either: Start would pause it.
    if (!s.open) return chord;

    uint16_t now = buttons & uint16_t(kUp | kDown | kLeft | kRight | kLB | kRB | kA | kB);
    Stick(axes, now);
    const uint16_t pressed = uint16_t(now & ~s.held);
    const auto time = Clock::now();
    // A held direction repeats: after a pause, then steadily.
    uint16_t steps = pressed & uint16_t(kUp | kDown | kLeft | kRight);
    const uint16_t directions = now & uint16_t(kUp | kDown | kLeft | kRight);
    if (pressed & directions) s.repeatAt = time + std::chrono::milliseconds(380);
    else if (directions && time >= s.repeatAt)
    {
        steps = directions;
        s.repeatAt = time + std::chrono::milliseconds(110);
    }
    s.held = now;
    if (chord) return true;

    const int categories = int(kCategories.size());
    if (pressed & kB) { s.open = false; g_open = false; LOGI("menu: closed"); return true; }
    if (pressed & kLB) { s.category = (s.category + categories - 1) % categories; s.row = -1; }
    if (pressed & kRB) { s.category = (s.category + 1) % categories; s.row = -1; }
    const std::vector<int> entries = Entries(s.category);
    if (entries.empty()) return true;
    if (s.row < 0) s.row = Selectable(entries, 0, 1);
    if (steps & kUp) s.row = Selectable(entries, s.row - 1, -1);
    if (steps & kDown) s.row = Selectable(entries, s.row + 1, 1);
    Setting& setting = s.settings[entries[size_t(s.row)]];
    if (steps & kLeft) Step(setting, -1);
    if (steps & kRight) Step(setting, 1);
    if (pressed & kA) Activate(setting);
    return true;
}

menu::View menu::Snapshot()
{
    std::lock_guard held(s.lock);
    View view;
    view.open = s.open;
    if (!s.open) return view;
    for (const Category& category : kCategories) view.categories.push_back(category.name);
    view.category = s.category;
    view.categoryDescription = kCategories[size_t(s.category)].description;
    const std::vector<int> entries = Entries(s.category);
    view.row = std::max(s.row, 0);
    for (int index : entries)
    {
        const Setting& setting = s.settings[size_t(index)];
        Row row;
        row.label = setting.label;
        row.description = setting.description;
        row.header = setting.header;
        row.action = !setting.choices.size();
        row.restart = setting.restart;
        if (!setting.choices.empty())
        {
            row.value = setting.choices[size_t(setting.chosen)].label;
            row.canLeft = setting.chosen > 0;
            row.canRight = setting.chosen + 1 < int(setting.choices.size());
        }
        view.rows.push_back(std::move(row));
    }
    if (!s.notice.empty() && Clock::now() - s.noticeAt < std::chrono::seconds(6)) view.notice = s.notice;
    view.restartPending = s.restartPending;
    return view;
}

void menu::ApplyAtStart(const std::filesystem::path& folder)
{
    const bool maxRank = env::Flag("MW2_CHEAT_MAX_RANK");
    const char* prestigeText = env::Text("MW2_CHEAT_PRESTIGE");
    const bool unlockAll = env::Flag("MW2_CHEAT_UNLOCK_ALL");
    if (!maxRank && !prestigeText && !unlockAll) return;

    std::string done;
    bool any = false;
    for (const profile::Player& player : profile::Players())
    {
        if (!player.first) continue;
        any = true;
        std::string error;
        if (prestigeText && !profile::SetPrestige(player, std::atoi(prestigeText), error))
            LOGW("menu: prestige: %s", error.c_str());
        if (maxRank && !profile::MaxRank(player, error)) LOGW("menu: rank: %s", error.c_str());
        if (unlockAll && !profile::UnlockEverything(player, error)) LOGW("menu: unlocks: %s", error.c_str());
        done = player.name;
    }
    if (!any)
    {
        // A profile has no stats file until its first match ends. The switches
        // stay, and are tried again at the next start.
        LOGW("menu: no saved multiplayer stats yet; the cheats wait until a match has been played");
        std::lock_guard held(s.lock);
        Notice("Cheats wait until you have finished one match");
        return;
    }
    LOGI("menu: cheats applied to %s's stats", done.c_str());
    const std::filesystem::path file = folder / ".env";
    settings::Store(file, "MW2_CHEAT_MAX_RANK", "");
    settings::Store(file, "MW2_CHEAT_PRESTIGE", "");
    settings::Store(file, "MW2_CHEAT_UNLOCK_ALL", "");
    std::lock_guard held(s.lock);
    Notice("Cheats applied to " + done + "'s stats");
}
