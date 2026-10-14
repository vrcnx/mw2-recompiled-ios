#pragma once
// The shape of screen the title is told it has. MW2 lays out its view and its
// HUD for a 4:3 television when the console says the display is not
// widescreen, still drawing into its 1280x720 buffer, which the console's
// scaler then stretched to the set. An iPad's screen is 4:3 (the 11-inch's a
// little wider), so there the title is told so and the frame fills the
// screen, where a 16:9 picture left a third of it black.
//
// MW2_ASPECT=4:3 or 16:9 (R); unset, 4:3 on iOS and 16:9 elsewhere.
#include <cstring>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include "env.h"

namespace display
{
    inline bool FourByThree()
    {
        static const bool narrow = [] {
            if (const char* aspect = env::Text("MW2_ASPECT"))
                return std::strcmp(aspect, "4:3") == 0;
#if defined(__APPLE__) && TARGET_OS_IOS
            return true;
#else
            return false;
#endif
        }();
        return narrow;
    }
}
