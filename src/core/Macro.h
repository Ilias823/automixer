#pragma once

// The "Amount" macro: one control that sets the detail controls from gentle
// (0 %) to aggressive (100 %). 50 % gives exactly the factory defaults.
//
//   gentle:     other mics drop less, the winner is chosen more softly, and
//               mics stay open longer after speech (fewer cut word ends)
//   aggressive: other mics drop further, the loudest mic wins harder, and
//               mics close quickly (more bleed removed)

#include <algorithm>

namespace am
{

struct MacroValues
{
    float rangeDb, sharpness, holdMs, releaseMs;
};

namespace detail
{
// Piecewise linear through (0, lo), (0.5, mid), (1, hi).
inline float threePoint (float t, float lo, float mid, float hi)
{
    t = std::clamp (t, 0.0f, 1.0f);
    return t < 0.5f ? lo + (mid - lo) * (t * 2.0f) : mid + (hi - mid) * ((t - 0.5f) * 2.0f);
}
} // namespace detail

// amountPercent: 0..100
inline MacroValues macroValues (float amountPercent)
{
    const float t = amountPercent / 100.0f;
    return {
        detail::threePoint (t, 8.0f, 18.0f, 30.0f),   // range dB
        detail::threePoint (t, 1.5f, 3.0f, 4.0f),     // focus
        detail::threePoint (t, 300.0f, 0.0f, 0.0f),   // hold ms
        detail::threePoint (t, 250.0f, 80.0f, 40.0f), // release ms
    };
}

} // namespace am
