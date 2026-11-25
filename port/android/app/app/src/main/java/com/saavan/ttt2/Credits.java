package com.saavan.ttt2;

/** The CREDITS page (r23: Catonix's profile card, as in the WWE 2K16 port). Edit here. */
final class Credits {
    private Credits() {}

    /** The Android port's author, shown on the credits card and in the home screen's corner. */
    static final String PORT_BY = "CATONIX";
    static final String SUBTITLE = "Port of Tekken Tag Tournament 2 to Android";

    /** Profile photo: res/drawable-nodpi/catonix.png. Links: label, address, note. */
    static final String[][] LINKS = {
            {"YouTube", "https://youtube.com/@catonix-ru7", ""},
            {"Website", "http://volvisionmods.com", ""},
            {"Telegram", "https://t.me/catonixports", "(updates, help, send logs)"},
    };
    /** The card's buttons: label, address. */
    static final String[][] BUTTONS = {
            {"Open YouTube Channel", "https://youtube.com/@catonix-ru7"},
            {"Open Website", "http://volvisionmods.com"},
            {"Telegram Channel", "https://t.me/catonixports"},
    };

    /** The work this app is built on. */
    static final String[][] THANKS = {
            {"Tekken Tag Tournament 2 PC recompilation", "shihabal3amri and contributors - the PC port this Android version is built on"},
            {"XenonRecomp", "hedge-dev: the Xbox 360 static recompiler (MIT)"},
            {"References", "Unleashed Recompiled (hedge-dev) and the Xenia project"},
            {"DXVK native", "Philip Rebohle, Joshua Ashton and contributors: Direct3D 11 on Vulkan (zlib licence)"},
            {"dxvk-async", "Sporif and contributors: background pipeline compilation (zlib licence)"},
            {"vkd3d-shader", "The Wine project: the HLSL shader compiler (LGPL)"},
            {"SDL 2", "Window, input and audio (zlib licence)"},
            {"FFmpeg", "XMA audio and video decoding (LGPL)"},
            {"libmspack, tiny-AES-c", "LZX decompression (LGPL) and AES"},
            {"Oswald font", "The Oswald Project Authors (SIL Open Font Licence)"},
    };

    static final String DISCLAIMER = "Unofficial fan project, free and non-commercial. Not affiliated with or endorsed by "
            + "Bandai Namco or Microsoft. No game files are included: you need your own Tekken Tag Tournament 2 (Xbox 360) "
            + "disc image and Title Update. Tekken and all related characters and content are the property of their respective owners.";
}
