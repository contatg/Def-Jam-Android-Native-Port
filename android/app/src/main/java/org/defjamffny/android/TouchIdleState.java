package org.defjamffny.android;

/** UI-thread idle state. A held finger is activity, even with no MOVE events. */
final class TouchIdleState {
    static final long TIMEOUT_MS = 15000;
    private long last;
    private int fingers;
    private boolean hidden;
    void reset(long now){last=now;fingers=0;hidden=false;}
    void touch(long now,int count){last=now;fingers=Math.max(0,count);hidden=false;}
    void keepVisible(long now){last=now;hidden=false;}
    void advance(long now){if(fingers==0&&now>=last&&now-last>=TIMEOUT_MS)hidden=true;}
    boolean hidden(){return hidden;}
}
