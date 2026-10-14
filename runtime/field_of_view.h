#pragma once
// The field of view the title is made to draw with (field_of_view.cpp).
namespace fov
{
    // Degrees as cg_fov counts them, 65 (the console's) to 120; from the next frame.
    void Set(double degrees);
}
