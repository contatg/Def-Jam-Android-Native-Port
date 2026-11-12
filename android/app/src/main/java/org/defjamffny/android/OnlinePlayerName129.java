package org.defjamffny.android;

import android.content.SharedPreferences;

/** Same key as the previous lobby; no account or network identity migration. */
final class OnlinePlayerName129 {
    static String normalize(String value) { return value == null ? "" : value.trim(); }
    static String read(SharedPreferences preferences) {
        String value = normalize(preferences.getString("name", "Player"));
        return OnlineMenuState.validName(value) ? value : "Player";
    }
    static boolean save(SharedPreferences preferences, String value) {
        String normalized = normalize(value);
        return OnlineMenuState.validName(normalized)
                && preferences.edit().putString("name", normalized).commit();
    }
    private OnlinePlayerName129() { }
}
