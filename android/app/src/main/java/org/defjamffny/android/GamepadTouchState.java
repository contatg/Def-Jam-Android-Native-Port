package org.defjamffny.android;

import java.util.HashSet;
import java.util.Set;
import java.util.function.IntPredicate;

/** UI-thread state. A removal wins over an out-of-date device inventory. */
final class GamepadTouchState {
    private final Set<Integer> removed = new HashSet<>();
    private boolean touchRequested;

    void deviceAdded(int id, boolean gamepad) {
        removed.remove(id);
        if (gamepad) touchRequested = false;
    }

    void deviceRemoved(int id) { removed.add(id); }

    void requestTouch() { touchRequested = true; }

    boolean useGamepad(int id) {
        // Late events from a removed device must not re-hide the touch pad.
        if (removed.contains(id)) return false;
        touchRequested = false;
        return true;
    }

    boolean shouldHideTouch(int[] inventory, IntPredicate isPhysicalGamepad) {
        if (touchRequested) return false;
        for (int id : inventory) {
            if (!removed.contains(id) && isPhysicalGamepad.test(id)) return true;
        }
        return false;
    }
}
