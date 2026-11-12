package org.defjamffny.android;

/** Stabilises only the touch-overlay choice; it never delays guest input. */
final class InputModeDebounce35 {
    private static final long MENU_STABLE_MS=1000, PAUSE_INTENT_MS=2000;
    private int current=-1;
    private long menuSince=-1, pauseUntil=-1;
    void pauseIntent(long now){if(current==1)pauseUntil=now+PAUSE_INTENT_MS;}
    int choose(int proposed,long now){
        if(proposed==2||proposed==3){current=proposed;menuSince=-1;pauseUntil=-1;return current;}
        if(proposed==1){current=1;menuSince=-1;return current;}
        if(current!=1){current=0;menuSince=-1;return current;}
        if(pauseUntil>=now){current=0;menuSince=-1;pauseUntil=-1;return current;}
        if(menuSince<0||now<menuSince){menuSince=now;return 1;}
        if(now-menuSince<MENU_STABLE_MS)return 1;
        current=0;menuSince=-1;return current;
    }
}
