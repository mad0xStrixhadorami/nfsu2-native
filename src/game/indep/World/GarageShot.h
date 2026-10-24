#pragma once

// Where the garage camera looks while a shop screen is open (ISSUES #285): like Most Wanted's customisation, the camera
// glides to the side of the car that the part being browsed lives on. The table of poses is in CameraRig.h.
enum class GarageShot : int {
    Overview,    // the swaying showroom view (crib menu, category carousels)
    FrontBumper, // front bumper / front end
    RearBumper,
    Exhaust,     // low rear corner
    Skirt,       // side sills, wide body
    Spoiler,     // high rear three-quarter
    Hood,        // high front three-quarter (hoods, performance engine parts)
    Roof,
    Headlight,   // close on a front corner
    Taillight,   // close on a rear corner
    Wheel,       // front wheel close-up (rims, spinners, brakes)
    Underglow,   // low side view
    Side,        // paint, vinyls, decals, tint: the full side profile
    Count
};
