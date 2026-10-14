#pragma once
// The settings menu: this port's own, over the game, as the console's guide
// is. RB and Start together open and close it; while it is open the first
// controller drives it and the title sees nothing pressed (kernel/input.cpp).
//
// Each entry is a switch of the settings file (`.env` in the player's folder,
// runtime/settings.cpp) and is written there when it changes, so it holds at
// the next start. Some take effect at once (the field of view, the frame
// counter); the rest are read when the game starts, and say so. The cheats
// change the saved stats (launcher/profile.h), which the game reads once, so
// they are done at the next start, before the title is entered
// (ApplyAtStart).
//
// The state is the input's; the presenter draws a copy of it (Snapshot).
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace menu
{
    // From the first controller's every read (XInputGetState). True when the
    // title is not to see what it holds: the menu is open, or the chord that
    // opens it is held.
    bool Input(uint16_t buttons, const int16_t axes[4]);
    bool Open();

    // What the presenter draws, copied under the menu's lock.
    struct Row
    {
        std::string label, value, description;
        bool header = false;        // a heading between rows, not chosen
        bool action = false;        // A does it; no value to step through
        bool restart = false;       // takes effect at the next start
        bool canLeft = false, canRight = false;
    };
    struct View
    {
        bool open = false;
        std::vector<std::string> categories;
        int category = 0;
        std::string categoryDescription;
        std::vector<Row> rows;
        int row = 0;
        std::string notice;         // the last thing done, for a few seconds
        bool restartPending = false;
    };
    View Snapshot();

    // The frame counter, drawn over the game while the menu is shut.
    bool FrameCounter();

    // Before the guest is entered: the cheats chosen in the last run, done to
    // the saved stats in `folder`, and their switches cleared.
    void ApplyAtStart(const std::filesystem::path& folder);
    // Where the settings file is, from main.
    void SetSettingsFile(const std::filesystem::path& path);
}
