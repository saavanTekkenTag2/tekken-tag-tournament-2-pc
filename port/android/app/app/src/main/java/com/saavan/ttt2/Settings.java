package com.saavan.ttt2;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.BufferedReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Every setting the launcher's OPTIONS page shows (r22). Three kinds of storage:
 *  - INI: the game's own settings.ini (TTT2Recomp/settings.ini, display_settings.cpp), the same keys the in-game
 *    Options menu writes; the game reads them when it starts.
 *  - ENV: a switch the runtime reads from its environment, passed to libmain.so as "--set NAME=VALUE"
 *    (GameActivity -> android_main.cpp).
 *  - PREF: the app's own (on-screen controls, performance mode, launcher animations).
 */
final class Settings {
    private Settings() {}

    static final int INI = 0, ENV = 1, PREF = 2;

    static final class Opt {
        final String group, label, help;
        final int store;
        final String section, key;       // INI: [section] key; ENV: key = variable name; PREF: key = preference name
        final String[] values, labels;
        final String def;
        Opt(String group, String label, String help, int store, String section, String key, String def, String[] values, String[] labels) {
            this.group = group; this.label = label; this.help = help; this.store = store; this.section = section; this.key = key;
            this.def = def; this.values = values; this.labels = labels;
        }
        int index(String v) {
            for (int i = 0; i < values.length; i++) if (values[i].equals(v)) return i;
            for (int i = 0; i < values.length; i++) if (values[i].equals(def)) return i;
            return 0;
        }
    }

    private static final String[] ONOFF_V = { "1", "0" }, ONOFF_L = { "ON", "OFF" };
    private static String[] range(int lo, int hi) { String[] r = new String[hi - lo + 1]; for (int i = lo; i <= hi; i++) r[i - lo] = String.valueOf(i); return r; }
    private static String[] rangeLabels(int lo, int hi, String zero) {
        String[] r = range(lo, hi);
        for (int i = 0; i < r.length; i++) if (zero != null && r[i].equals("0")) r[i] = zero; else if (lo < 0 && lo + i > 0) r[i] = "+" + r[i];
        return r;
    }
    private static Opt ini(String g, String label, String help, String sec, String key, String def, String[] v, String[] l) { return new Opt(g, label, help, INI, sec, key, def, v, l); }
    private static Opt env(String g, String label, String help, String name, String def, String[] v, String[] l) { return new Opt(g, label, help, ENV, null, name, def, v, l); }
    private static Opt pref(String g, String label, String help, String name, String def, String[] v, String[] l) { return new Opt(g, label, help, PREF, null, name, def, v, l); }

    static final List<Opt> ALL = new ArrayList<>();
    static {
        // ---------------- DISPLAY (settings.ini [Display], the in-game Options -> Display Settings)
        String D = "DISPLAY";
        ALL.add(ini(D, "Resolution", "The size the game is drawn at. Lower = faster. 540p is a good start for mid-range phones.",
            "Display", "Resolution", "960x540",
            new String[] { "640x360", "960x540", "1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160" },
            new String[] { "360p (fastest)", "540p", "720p", "900p", "1080p", "1440p", "4K (slowest)" }));
        ALL.add(ini(D, "Anti-aliasing", "Smooths jagged edges. SSAA draws the game 1.5x / 2x larger: very slow on phones.",
            "Display", "AntiAliasing", "0", range(0, 3), new String[] { "OFF", "SMAA", "SSAA 1.5X", "SSAA 2X" }));
        ALL.add(ini(D, "Sharpness", "Contrast adaptive sharpening of the scaled picture (helps lower resolutions).",
            "Display", "Sharpness", "0", range(0, 10), rangeLabels(0, 10, "OFF")));
        ALL.add(pref(D, "Graphics fix", "Turn ON if textures look wrong, are missing or clothes look white on your phone. "
            + "It uses the safest graphics path: a little slower.", "gfx_compat", "0", ONOFF_V, ONOFF_L));
        ALL.add(ini(D, "Texture filtering", "Standard is the console's look. Anisotropic is sharper at an angle, a little slower.",
            "Display", "TextureFiltering", "0", range(0, 5), new String[] { "STANDARD", "TRILINEAR", "ANISO 2X", "ANISO 4X", "ANISO 8X", "ANISO 16X" }));
        ALL.add(ini(D, "VSync", "ON: no tearing. OFF: a little less input lag, the picture may tear.",
            "Display", "VSync", "1", ONOFF_V, ONOFF_L));
        ALL.add(ini(D, "Brightness", "A gamma curve on the final picture. 0 = as the game draws it.",
            "Display", "Brightness", "0", range(-5, 5), rangeLabels(-5, 5, "0")));
        ALL.add(ini(D, "Low latency", "Shows each frame as soon as it is ready. AUTO decides by the device.",
            "Display", "LowLatencyMode", "2", new String[] { "0", "1", "2" }, new String[] { "AUTO", "ON", "OFF" }));
        ALL.add(ini(D, "FPS counter", "Frames per second in a corner of the screen.",
            "Display", "FpsCounter", "0", range(0, 4), new String[] { "OFF", "TOP LEFT", "TOP RIGHT", "BOTTOM LEFT", "BOTTOM RIGHT" }));
        ALL.add(ini(D, "Shadows", "Character and stage shadows. OFF is faster.", "Display", "Shadows", "0", ONOFF_V, ONOFF_L));
        ALL.add(ini(D, "Depth of field", "Background blur in cut-scenes and menus. OFF is faster.", "Display", "DepthOfField", "1", ONOFF_V, ONOFF_L));
        ALL.add(ini(D, "Bloom", "Glow around bright light. OFF is faster.", "Display", "Bloom", "1", ONOFF_V, ONOFF_L));
        ALL.add(ini(D, "Light shafts", "Rays of light on some stages. OFF is faster.", "Display", "LightShafts", "1", ONOFF_V, ONOFF_L));
        ALL.add(ini(D, "Weather", "Rain, snow and particles on some stages. OFF is faster.", "Display", "Weather", "1", ONOFF_V, ONOFF_L));

        // ---------------- GAME
        String G = "GAME";
        ALL.add(ini(G, "Quick start", "Skips the logos and the intro movie, straight to Press Start.", "Game", "QuickStart", "0", ONOFF_V, ONOFF_L));
        ALL.add(ini(G, "Input delay", "The console's original 2 frames of input delay offline. OFF = more responsive.", "Game", "InputDelay", "0", ONOFF_V, ONOFF_L));
        ALL.add(env(G, "Auto sign-in", "Signs in a local profile at start so saving works.", "TTT2_AUTO_SIGNIN", "1", ONOFF_V, ONOFF_L));

        // ---------------- ONLINE
        String O = "ONLINE";
        ALL.add(ini(O, "Netcode", "How online matches trade delay for rollback corrections.", "Online", "Netcode", "0", range(0, 3),
            new String[] { "STANDARD", "PRIORITIZE RESPONSE", "PRIORITIZE GRAPHICS", "CUSTOM" }));
        ALL.add(ini(O, "Custom delay", "Frames of input delay (Netcode CUSTOM).", "Online", "CustomDelay", "1", range(1, 6), range(1, 6)));
        ALL.add(ini(O, "Custom rollback", "Most frames corrected by rollback (Netcode CUSTOM).", "Online", "CustomRollback", "8", range(1, 8), range(1, 8)));
        ALL.add(ini(O, "Rematch menu", "Offer a rematch after an online match.", "Online", "RematchMenu", "1", ONOFF_V, ONOFF_L));

        // ---------------- PERFORMANCE
        String P = "PERFORMANCE";
        ALL.add(env(P, "Renderer", "D3D11 (DXVK): the tested one. NATIVE VULKAN (EXPERIMENTAL): the game's graphics straight to Vulkan, "
            + "no DXVK in between - may show problems; if it cannot start, DXVK is used.", "TTT2_RENDERER", "native",
            new String[] { "", "native" }, new String[] { "D3D11 (DXVK)", "NATIVE VULKAN (TEST)" }));
        ALL.add(env(P, "Native: background pipelines", "Native Vulkan only: new effects on screens already showing are prepared in the "
            + "background (no stutter; the effect may appear a frame late).", "TTT2_NATIVE_ASYNC", "1", ONOFF_V, ONOFF_L));
        ALL.add(env(P, "Native: pipeline threads", "Threads the native renderer uses to build new effects in the background (more = effects appear sooner at match start).",
            "TTT2_NATIVE_PIPELINE_THREADS", "3", new String[] { "1", "2", "3", "4" }, new String[] { "1", "2", "3", "4" }));
        ALL.add(env(P, "Lazy readbacks", "Effects that copy the picture back each frame use the previous frame's copy instead of waiting for "
            + "the GPU (smoother menus). OFF if an effect looks a frame late.", "TTT2_LAZY_READBACK", "1", ONOFF_V, ONOFF_L));
        ALL.add(env(P, "Render scale", "Overrides Resolution: the game drawn at this fraction of 720p. AUTO follows Resolution.", "TTT2_RENDER_SCALE", "",
            new String[] { "", "0.5", "0.6", "0.67", "0.75", "0.85", "1.0", "1.25", "1.5" },
            new String[] { "AUTO", "50%", "60%", "67%", "75%", "85%", "100%", "125%", "150%" }));
        ALL.add(env(P, "Async pipelines", "Vulkan pipelines are built in the background: no freeze when a new effect appears (it may pop in a frame late).",
            "DXVK_ASYNC", "1", ONOFF_V, ONOFF_L));
        ALL.add(env(P, "Async shaders", "New game shaders compile in the background instead of stopping the game.", "TTT2_SHADER_ASYNC", "1", ONOFF_V, ONOFF_L));
        ALL.add(env(P, "Shader build threads", "How many shaders compile at once (on the slow cores). AUTO picks by the phone.", "TTT2_SHADER_THREADS", "",
            new String[] { "", "1", "2", "3", "4" }, new String[] { "AUTO", "1", "2", "3", "4" }));
        ALL.add(env(P, "Vulkan pipeline threads", "Threads DXVK uses to build pipelines. Fewer = smoother menus, more = shorter first loads.", "TTT2_DXVK_THREADS", "2",
            new String[] { "1", "2", "3", "4", "6" }, new String[] { "1", "2", "3", "4", "6" }));
        ALL.add(env(P, "Shader warm-up", "Loads every shader built before at start: longer loading, fewer stutters.", "TTT2_SHADER_WARM", "1", ONOFF_V, ONOFF_L));
        ALL.add(env(P, "Big cores", "Keeps the game on the phone's fast cores and the background work on the slow ones.", "TTT2_BIG_CORES", "1", ONOFF_V, ONOFF_L));
        ALL.add(env(P, "GPU vertex shaders", "Characters' vertices on the GPU (OFF: the CPU, much slower; for testing).", "TTT2_GPU_VS", "1", ONOFF_V, ONOFF_L));
        ALL.add(env(P, "Dynamic resolution", "The console's own resolution drop in heavy scenes. ON can help weak phones.", "TTT2_DYNRES", "",
            new String[] { "", "1" }, new String[] { "OFF", "ON" }));
        ALL.add(env(P, "Frame overlap", "EXPERIMENTAL: the next frame's game logic runs while the GPU draws this one.", "TTT2_FRAME_OVERLAP", "0",
            new String[] { "0", "1" }, new String[] { "OFF", "ON" }));
        ALL.add(env(P, "Frame priority", "The game's main thread and renderer above everything else.", "TTT2_FRAME_PRIORITY", "1",
            new String[] { "1", "0" }, new String[] { "ON", "OFF" }));
        ALL.add(env(P, "Present interval", "Overrides VSync: 1 = every refresh, 0 = at once.", "TTT2_PRESENT_INTERVAL", "",
            new String[] { "", "0", "1", "2" }, new String[] { "AUTO", "0", "1", "2" }));
        ALL.add(env(P, "Vertex data cache", "Keeps unchanged vertex data on the GPU between frames.", "TTT2_VDATA_CACHE", "1", ONOFF_V, ONOFF_L));
        ALL.add(env(P, "Index cache", "Keeps unchanged index lists on the GPU between frames.", "TTT2_INDEX_CACHE", "1", ONOFF_V, ONOFF_L));
        ALL.add(env(P, "Texture memo", "Skips re-uploading textures that did not change.", "TTT2_TEX_MEMO", "1", ONOFF_V, ONOFF_L));
        ALL.add(env(P, "Fast untile", "Faster texture conversion.", "TTT2_FAST_UNTILE", "1", ONOFF_V, ONOFF_L));
        ALL.add(pref(P, "Sustained performance", "Asks Android for steady clocks instead of short bursts (less throttling in long sessions).",
            "sustained", "1", ONOFF_V, ONOFF_L));
        ALL.add(pref(P, "Display refresh", "Asks the screen for 60 Hz while playing (steadier pacing on 90/120 Hz screens).",
            "fps60", "1", ONOFF_V, ONOFF_L));
        ALL.add(env(P, "GPU self-test at start", "A few small render checks at start (log only). OFF starts a little faster.", "TTT2_GPU_SELFTEST", "0", ONOFF_V, ONOFF_L));

        // ---------------- CONTROLS
        String C = "CONTROLS";
        ALL.add(pref(C, "On-screen controls", "The touch pad over the game.", "touch", "1", ONOFF_V, ONOFF_L));
        ALL.add(pref(C, "Full screen", "Uses the whole screen, also beside the camera hole.", "full_screen", "1", ONOFF_V, ONOFF_L));
        ALL.add(pref(C, "Controls opacity", "How see-through the touch pad is.", "touch_alpha", "100",
            new String[] { "30", "45", "60", "75", "90", "100" }, new String[] { "30%", "45%", "60%", "75%", "90%", "100%" }));
        ALL.add(pref(C, "Controls size", "The size of the touch pad.", "touch_size", "100",
            new String[] { "70", "80", "90", "100", "115", "130" }, new String[] { "70%", "80%", "90%", "100%", "115%", "130%" }));

        // ---------------- LAUNCHER
        String L = "LAUNCHER";
        ALL.add(pref(L, "Remove animations", "Turns off the launcher's motion (the menu appears at once).", "no_anim", "0", ONOFF_V, ONOFF_L));

        // ---------------- DEBUG
        String X = "DEBUG";
        ALL.add(env(X, "DXVK overlay", "DXVK's own on-screen info.", "DXVK_HUD", "0",
            new String[] { "0", "fps", "fps,frametimes", "fps,frametimes,gpuload,pipelines,memory" }, new String[] { "OFF", "FPS", "FPS + FRAME TIMES", "FULL" }));
        ALL.add(env(X, "DXVK log level", "How much DXVK writes to its logs.", "DXVK_LOG_LEVEL", "warn",
            new String[] { "none", "error", "warn", "info", "debug" }, new String[] { "NONE", "ERROR", "WARN", "INFO", "DEBUG" }));
        ALL.add(env(X, "Frame times log", "Writes every frame's time to the log (for slowdown reports).", "TTT2_FRAMETIME", "",
            new String[] { "", "1" }, new String[] { "OFF", "ON" }));
        ALL.add(env(X, "Hitch profile", "Logs what the game was doing during each long frame.", "TTT2_HITCH_PROFILE", "",
            new String[] { "", "1" }, new String[] { "OFF", "ON" }));
        ALL.add(env(X, "Movie trace", "Logs the movie player's threads (for freeze reports).", "TTT2_MOVIE_TRACE", "",
            new String[] { "", "1" }, new String[] { "OFF", "ON" }));
        ALL.add(pref(X, "Save frames", "Saves the rendered frame once a second (slower). View with FRAMES.", "fbdump", "0", ONOFF_V, ONOFF_L));
        ALL.add(pref(X, "CPU renderer", "The slow software renderer, to tell GPU problems from game problems.", "cpu", "0", ONOFF_V, ONOFF_L));
    }

    static final String[] GROUPS = { "DISPLAY", "GAME", "ONLINE", "PERFORMANCE", "CONTROLS", "LAUNCHER", "DEBUG" };

    // ---------------------------------------------------------------- r28: locked settings
    /**
     * The settings players can change. Everything else is locked to the tested value (whatever an older build saved),
     * so a setting cannot make the game slow or unstable. The developer unlock (credits: tap the version 7 times)
     * shows every setting again for testing.
     */
    static final Set<String> BASIC = new HashSet<>(Arrays.asList(
            "Resolution", "gfx_compat", "VSync", "Brightness", "FpsCounter", "Sharpness", "Shadows", "DepthOfField", "Bloom", "LightShafts", "Weather",
            "QuickStart", "InputDelay",
            "Netcode", "CustomDelay", "CustomRollback", "RematchMenu",
            "touch", "full_screen", "touch_alpha", "touch_size",
            "no_anim"));
    /** Resolution choices when locked: 360p, 540p, 720p (900p and up are too slow on phones). */
    private static final int BASIC_RESOLUTIONS = 3;
    /** r29: Mali GPUs: 360p and 540p (720p is too slow on them). */
    private static final int MALI_RESOLUTIONS = 2;
    static final Set<String> HIDDEN_GROUPS = new HashSet<>(Arrays.asList("PERFORMANCE", "DEBUG"));

    static boolean devUnlocked(Context c) { return c.getSharedPreferences("ttt2", Context.MODE_PRIVATE).getBoolean("dev_unlock", false); }
    static void setDevUnlocked(Context c, boolean on) { c.getSharedPreferences("ttt2", Context.MODE_PRIVATE).edit().putBoolean("dev_unlock", on).apply(); }
    static boolean locked(Context c, Opt o) { return !BASIC.contains(o.key) && !devUnlocked(c); }
    static boolean groupVisible(Context c, String g) { return devUnlocked(c) || !HIDDEN_GROUPS.contains(g); }
    /** How many of an option's values can be picked. */
    static int choices(Context c, Opt o) {
        if (o.key.equals("Resolution") && o.store == INI && !devUnlocked(c))
            return Math.min(GpuInfo.isMali(c) ? MALI_RESOLUTIONS : BASIC_RESOLUTIONS, o.values.length);
        return o.values.length;
    }

    /**
     * Puts every locked setting back to its tested value (launcher start and PLAY), and applies the release defaults once
     * (540p, shadows off) for players who never chose them.
     */
    static void enforce(Context c) {
        if (devUnlocked(c)) return;
        LinkedHashMap<String, LinkedHashMap<String, String>> ini = readIni(c);
        SharedPreferences p = c.getSharedPreferences("ttt2", Context.MODE_PRIVATE);
        SharedPreferences.Editor e = p.edit();
        boolean iniChanged = false;
        for (Opt o : ALL) {
            if (o.key.equals("Resolution") && o.store == INI) {
                String v = iniGet(ini, o.section, o.key);
                int i = -1;
                for (int k = 0; k < o.values.length; k++) if (o.values[k].equals(v)) i = k;
                if (v == null || i < 0 || i >= choices(c, o)) { iniPut(ini, o.section, o.key, o.def); iniChanged = true; }
                continue;
            }
            if (!locked(c, o)) {
                if (o.store == INI && !p.getBoolean("release_defaults_1", false) && iniGet(ini, o.section, o.key) == null) {
                    iniPut(ini, o.section, o.key, o.def);
                    iniChanged = true;
                }
                continue;
            }
            if (o.store == INI) {
                if (!o.def.equals(iniGet(ini, o.section, o.key))) { iniPut(ini, o.section, o.key, o.def); iniChanged = true; }
            } else if (o.store == ENV) e.remove("env_" + o.key);
            else e.remove(o.key);
        }
        e.putBoolean("release_defaults_1", true);
        // r29: Mali GPUs start lighter once (360p, no shadows, depth of field or light shafts); players can turn them on.
        if (!p.getBoolean("mali_defaults_1", false) && GpuInfo.isMali(c)) {
            iniPut(ini, "Display", "Resolution", "640x360");
            iniPut(ini, "Display", "Shadows", "0");
            iniPut(ini, "Display", "DepthOfField", "0");
            iniPut(ini, "Display", "LightShafts", "0");
            iniChanged = true;
        }
        e.putBoolean("mali_defaults_1", true);
        e.apply();
        if (iniChanged) writeIni(c, ini);
    }

    // ---------------------------------------------------------------- storage
    static File iniFile(Context c) { return new File(LauncherActivity.dataRoot(c), "TTT2Recomp/settings.ini"); }

    /** settings.ini as [section] -> (key -> value), keeping the order and every key the launcher does not know. */
    static LinkedHashMap<String, LinkedHashMap<String, String>> readIni(Context c) {
        LinkedHashMap<String, LinkedHashMap<String, String>> out = new LinkedHashMap<>();
        File f = iniFile(c);
        if (!f.isFile()) return out;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
            String line, sec = "";
            while ((line = r.readLine()) != null) {
                String t = line.trim();
                if (t.isEmpty() || t.startsWith(";") || t.startsWith("#")) continue;
                if (t.startsWith("[") && t.endsWith("]")) { sec = t.substring(1, t.length() - 1).trim(); continue; }
                int eq = t.indexOf('=');
                if (eq <= 0) continue;
                String k = t.substring(0, eq).trim(), v = t.substring(eq + 1).trim();
                LinkedHashMap<String, String> m = null;
                for (Map.Entry<String, LinkedHashMap<String, String>> e : out.entrySet()) if (e.getKey().equalsIgnoreCase(sec)) m = e.getValue();
                if (m == null) { m = new LinkedHashMap<>(); out.put(sec, m); }
                String existing = null;
                for (String key : m.keySet()) if (key.equalsIgnoreCase(k)) existing = key;
                m.put(existing != null ? existing : k, v);
            }
        } catch (Exception ignored) {}
        return out;
    }

    static void writeIni(Context c, LinkedHashMap<String, LinkedHashMap<String, String>> ini) {
        File f = iniFile(c);
        //noinspection ResultOfMethodCallIgnored
        f.getParentFile().mkdirs();
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<String, LinkedHashMap<String, String>> s : ini.entrySet()) {
            sb.append('[').append(s.getKey()).append("]\r\n");
            for (Map.Entry<String, String> kv : s.getValue().entrySet()) sb.append(kv.getKey()).append('=').append(kv.getValue()).append("\r\n");
            sb.append("\r\n");
        }
        File tmp = new File(f.getPath() + ".tmp");
        try (FileOutputStream os = new FileOutputStream(tmp)) {
            os.write(sb.toString().getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) { return; }
        //noinspection ResultOfMethodCallIgnored
        tmp.renameTo(f);
    }

    static String iniGet(LinkedHashMap<String, LinkedHashMap<String, String>> ini, String sec, String key) {
        for (Map.Entry<String, LinkedHashMap<String, String>> s : ini.entrySet())
            if (s.getKey().equalsIgnoreCase(sec))
                for (Map.Entry<String, String> kv : s.getValue().entrySet()) if (kv.getKey().equalsIgnoreCase(key)) return kv.getValue();
        return null;
    }

    static void iniPut(LinkedHashMap<String, LinkedHashMap<String, String>> ini, String sec, String key, String value) {
        LinkedHashMap<String, String> m = null;
        for (Map.Entry<String, LinkedHashMap<String, String>> s : ini.entrySet()) if (s.getKey().equalsIgnoreCase(sec)) m = s.getValue();
        if (m == null) { m = new LinkedHashMap<>(); ini.put(sec, m); }
        String existing = null;
        for (String k : m.keySet()) if (k.equalsIgnoreCase(key)) existing = k;
        m.put(existing != null ? existing : key, value);
    }

    /** The current value of an option (its default if never set). */
    static String get(Context c, Opt o, LinkedHashMap<String, LinkedHashMap<String, String>> ini) {
        if (locked(c, o)) return o.def;
        SharedPreferences p = c.getSharedPreferences("ttt2", Context.MODE_PRIVATE);
        switch (o.store) {
            case INI: { String v = iniGet(ini, o.section, o.key); return v != null ? v : o.def; }
            case ENV: return p.getString("env_" + o.key, o.def);
            default: {
                // Older builds kept these as booleans / ints.
                Object v = p.getAll().get(o.key);
                if (v instanceof Boolean) return ((Boolean) v) ? "1" : "0";
                if (v instanceof Integer) return String.valueOf(v);
                if (v instanceof String) return (String) v;
                return o.def;
            }
        }
    }

    static void set(Context c, Opt o, String value, LinkedHashMap<String, LinkedHashMap<String, String>> ini) {
        SharedPreferences.Editor e = c.getSharedPreferences("ttt2", Context.MODE_PRIVATE).edit();
        switch (o.store) {
            case INI: iniPut(ini, o.section, o.key, value); writeIni(c, ini); return;
            case ENV: e.putString("env_" + o.key, value).apply(); return;
            default:
                if (o.values == ONOFF_V) e.putBoolean(o.key, "1".equals(value));
                else e.putInt(o.key, Integer.parseInt(value));
                e.apply();
        }
    }

    /** "--set NAME=VALUE" for every ENV option that differs from "" (unset = the runtime's own default), plus custom lines. */
    static List<String> envArgs(Context c) {
        SharedPreferences p = c.getSharedPreferences("ttt2", Context.MODE_PRIVATE);
        List<String> a = new ArrayList<>();
        for (Opt o : ALL) {
            if (o.store != ENV) continue;
            String v = locked(c, o) ? o.def : p.getString("env_" + o.key, o.def);
            if (v == null || v.isEmpty()) continue;
            a.add("--set"); a.add(o.key + "=" + v);
        }
        // r30: Graphics fix - every speed shortcut with a correctness risk off (on top of the locked values).
        if ("1".equals(p.getString("gfx_compat", "0"))) {
            for (String kv : new String[] { "TTT2_TEX_MEMO=0", "TTT2_FAST_UNTILE=0", "TTT2_LAZY_READBACK=0", "TTT2_NATIVE_FULL_BARRIERS=1",
                                            "TTT2_NATIVE_ONEOFF_WAIT=1", "TTT2_NATIVE_DECODE_BC=1", "TTT2_NATIVE_NO_SPV_CACHE=1",
                                            "TTT2_NATIVE_ASYNC=0", "TTT2_GFX_COMPAT=1" }) {
                a.add("--set"); a.add(kv);
            }
        }
        if (!devUnlocked(c)) return a;      // custom variables only with the developer unlock
        for (String line : p.getString("env_custom", "").split("\n")) {
            String t = line.trim();
            int eq = t.indexOf('=');
            if (t.isEmpty() || t.startsWith("#") || eq <= 0) continue;
            a.add("--set"); a.add(t);
        }
        return a;
    }

    /** Back to the defaults: settings.ini's known keys, every ENV switch and PREF (custom variables kept). */
    static void resetAll(Context c) {
        LinkedHashMap<String, LinkedHashMap<String, String>> ini = readIni(c);
        SharedPreferences.Editor e = c.getSharedPreferences("ttt2", Context.MODE_PRIVATE).edit();
        for (Opt o : ALL) {
            if (o.store == INI) iniPut(ini, o.section, o.key, o.def);
            else if (o.store == ENV) e.remove("env_" + o.key);
            else e.remove(o.key);
        }
        e.apply();
        writeIni(c, ini);
    }

    static boolean animationsOff(Context c) {
        boolean pref = c.getSharedPreferences("ttt2", Context.MODE_PRIVATE).getBoolean("no_anim", false);
        return pref || !android.animation.ValueAnimator.areAnimatorsEnabled();
    }
}
