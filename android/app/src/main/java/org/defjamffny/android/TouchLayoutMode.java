package org.defjamffny.android;

/** Saved touch profile override; native port overlay always keeps its own input. */
final class TouchLayoutMode {
    static final int AUTO=0, FIGHT=1, MENU=2;
    static int valid(int value) { return value>=AUTO&&value<=MENU?value:AUTO; }
    static int resolve(int automatic,int preference) {
        if(automatic==3) return 0; // Native online always uses the menu pad, even with a fight override.
        if(automatic==2) return 2;
        switch(valid(preference)) {
            case FIGHT:return 1;
            case MENU:return 0;
            default:return automatic==1?1:0;
        }
    }
    private TouchLayoutMode() { }
}
