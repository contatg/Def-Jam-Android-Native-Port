// SPDX-License-Identifier: GPL-3.0-or-later
package com.rfandango.haku_x;

/** JNI symbol owner required by the unmodified upstream 1.3.0 binary. */
public final class NativeCoreActivity {
    private NativeCoreActivity() {}

    public static native void nativeSetOverlayPad(int buttons, int analog,
            float lx, float ly, float rx, float ry, float lt, float rt);
    public static native void nativeSetPaused(boolean paused);
    public static native boolean nativeGetFrameShown();
    public static native float[] nativeGetFrameStats();
    public static native String nativeGetFatal();
    public static native int nativeGetFatalKind();
    public static native boolean nativeGetRelaunch();
    public static native boolean nativeGetQuit();
}
