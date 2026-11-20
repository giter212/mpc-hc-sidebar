#include "../src/mpc-hc/HoldSpeedGesture.h"
#include <cassert>
#include <iostream>

int main()
{
    HoldSpeedGesture hold;
    assert(!hold.Update(1000, 0, 240));
    hold.Begin(1000, 1.25);
    assert(!hold.Update(1449, 100, 240));
    assert(!hold.End()); // A short click must not change playback rate.
    hold.Begin(2000, 1.25);
    assert(hold.Update(2450, 100, 240) == 2.0);
    assert(hold.Update(2500, 180, 240) == 3.0);
    assert(!hold.Update(2550, 180, 240)); // No repeated graph updates at rest.
    assert(hold.Update(2600, 260, 240) == 4.0);
    assert(hold.Update(2650, 340, 240) == 5.0);
    assert(!hold.Update(2700, 2000, 240)); // The maximum stays at 5x.
    assert(hold.Update(2750, -100, 240) == 2.0);
    assert(hold.End() == 1.25); // Restore the exact speed that preceded the hold.
    assert(!hold.IsTracking() && !hold.IsActive());
    assert(!hold.End()); // Capture loss and button release can both end a hold.
    hold.Begin(3000, 0.75);
    assert(hold.Update(3450, 900, 480) == 2.0);
    assert(hold.Update(3500, 1060, 480) == 3.0); // Same physical gesture at 2x DPI.
    assert(hold.End() == 0.75);
    hold.Begin(4000, 1.0);
    assert(hold.Update(4450, 0, 0) == 2.0);
    assert(hold.Update(4500, 100, 0) == 5.0); // Defensive zero-width handling.
    assert(hold.End() == 1.0);
    std::cout << "Hold speed gesture tests passed\n";
}
