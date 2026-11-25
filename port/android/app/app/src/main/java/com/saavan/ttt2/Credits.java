package com.saavan.ttt2;

/** The CREDITS page (r22). Edit here. */
final class Credits {
    private Credits() {}

    /** The Android port's author, shown first and in the home screen's corner. */
    static final String PORT_BY = "SAVAN INDURKAR";
    static final String GITHUB = "github.com/Savanindurkar7";

    /** Catonix: shown with a profile photo (res/drawable-nodpi/catonix.jpg or .png; a badge when it is not there). */
    static final String CATONIX = "CATONIX";
    static final String[][] CATONIX_LINKS = {
            {"YOUTUBE", "https://youtube.com/@catonix-ru7"},
            {"WEBSITE", "http://volvisionmods.com"},
            {"TELEGRAM", "https://t.me/catonixports"},
    };

    /** Everyone whose work is in this app. */
    static final String[][] THANKS = {
            {"Android port", PORT_BY + " (" + GITHUB + ")"},
            {"Ports community", "CATONIX (YouTube, VolVision Mods, Telegram)"},
            {"Tekken Tag Tournament 2 PC recompilation", "shihabal3amri and contributors (github.com/shihabal3amri/tekken-tag-tournament-2-pc)"},
            {"XenonRecomp", "hedge-dev: the Xbox 360 static recompiler (MIT)"},
            {"References", "Unleashed Recompiled (hedge-dev) and the Xenia project"},
            {"DXVK native", "Philip Rebohle, Joshua Ashton and contributors: Direct3D 11 on Vulkan (zlib licence)"},
            {"dxvk-async", "Sporif and contributors: background pipeline compilation (zlib licence)"},
            {"vkd3d-shader", "The Wine project: HLSL shader compiler on Android (LGPL)"},
            {"SDL 2", "Window, input and audio (zlib licence)"},
            {"FFmpeg", "XMA audio and video decoding (LGPL)"},
            {"libmspack, tiny-AES-c", "LZX decompression (LGPL) and AES"},
            {"Oswald font", "The Oswald Project Authors (SIL Open Font Licence)"},
    };

    static final String DISCLAIMER = "Unofficial fan project, free and non-commercial. Not affiliated with or endorsed by "
            + "Bandai Namco or Microsoft. No game files are included: you need your own Tekken Tag Tournament 2 (Xbox 360) "
            + "disc image and Title Update. Tekken and all related characters and content are the property of their respective owners.";
}
