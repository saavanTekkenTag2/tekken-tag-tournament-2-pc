package com.saavan.ttt2;

/** Calls into libmain.so (port/android/runtime: android_install.cpp, android_main.cpp). */
public final class Native {
    static {
        System.loadLibrary("SDL2");
        System.loadLibrary("main");
    }

    public interface Listener {
        void progress(int percent, String status);
        boolean cancelled();
    }

    /** Builds the game folder under root from the disc image and Title Update 3. Null = success, else the error. */
    public static native String install(String isoPath, String titleUpdatePath, String root, Listener listener);

    /** r30: an extracted game folder (default.xex + data files) or an installed one (tekken.img + update/). Null = success. */
    public static native String installFolder(String[] names, String[] paths, String titleUpdatePath, String root, Listener listener);

    /** r30: the game's DLC packages found in folder, extracted to root/game/dlc. "OK:n" = installed, else the error. */
    public static native String installDlc(String folder, String root, Listener listener);

    /** r23: every game shader compiled into the cache ahead of time. Null = done, else the error / "stopped". */
    public static native String buildShaders(String root, Listener listener);

    /** On-screen controls: XInput button bits, sticks -32768..32767, triggers 0..255. */
    public static native void setTouchPad(int buttons, int lx, int ly, int rx, int ry, int lt, int rt);

    private Native() {}
}
