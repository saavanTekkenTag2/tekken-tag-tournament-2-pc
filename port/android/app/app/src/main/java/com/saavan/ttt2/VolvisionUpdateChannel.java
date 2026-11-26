package com.saavan.ttt2;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * Updates from volvisionmods.com (the site's update API, as in the WWE 2K16 port).
 *  - checkForUpdates: at every launcher start, in the background; a popup only when the site has a newer version.
 *  - checkNow: the home screen's CHECK FOR UPDATES - always answers (update, up to date, or could not reach the site).
 *  - fetch: the plain check, also used by UpdateJob for the notification while the app is closed.
 * To publish an update: upload the APK to the game page, set its latest version (e.g. 1.0.2, no "v") and What's new.
 */
public final class VolvisionUpdateChannel {
    private VolvisionUpdateChannel() {}

    static final String APP_ID = "nopZ9H8v2WlPzQmKBPAj";
    static final String STORE_URL = "https://volvisionmods.com/game/nopZ9H8v2WlPzQmKBPAj";
    private static final String API_URL = "https://volvision-api.syprusseal2.workers.dev/api/update-check?appId=" + APP_ID;

    /** What the site said. reached = false without internet or when the site does not answer. */
    static final class Result {
        boolean reached, newer;
        String latest = "", notes = "", url = STORE_URL;
    }

    /** Asks the site. Blocking: call it on a worker thread. */
    static Result fetch(String currentVersion) {
        Result r = new Result();
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(API_URL).openConnection();
            conn.setConnectTimeout(7000);
            conn.setReadTimeout(7000);
            conn.setRequestMethod("GET");
            conn.setRequestProperty("Accept", "application/json");
            conn.setUseCaches(false);
            if (conn.getResponseCode() == 200) {
                StringBuilder sb = new StringBuilder();
                try (BufferedReader reader = new BufferedReader(new InputStreamReader(conn.getInputStream()))) {
                    String line;
                    while ((line = reader.readLine()) != null) sb.append(line);
                }
                JSONObject json = new JSONObject(sb.toString());
                r.reached = true;
                r.latest = json.optString("latestVersion", "");
                r.notes = json.optString("whatsNew", "New features, improved performance, and bug fixes.");
                r.url = json.optString("storeUrl", STORE_URL);
                if (r.url == null || r.url.isEmpty()) r.url = STORE_URL;
                r.newer = !r.latest.isEmpty() && isNewerVersion(r.latest, currentVersion);
            }
        } catch (Exception e) {
            r.reached = false;
        } finally {
            if (conn != null) conn.disconnect();
        }
        return r;
    }

    /** Launcher start: a popup only when there is a newer version (silent when offline). */
    public static void checkForUpdates(final Activity activity, final String currentVersion) {
        check(activity, currentVersion, false);
    }

    /** CHECK FOR UPDATES: always answers. */
    public static void checkNow(final Activity activity, final String currentVersion) {
        Toast.makeText(activity, "Checking volvisionmods.com…", Toast.LENGTH_SHORT).show();
        check(activity, currentVersion, true);
    }

    private static void check(final Activity activity, final String currentVersion, final boolean manual) {
        new Thread(() -> {
            final Result r = fetch(currentVersion);
            new Handler(Looper.getMainLooper()).post(() -> {
                if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
                if (r.reached && r.newer) {
                    showDialog(activity, r.latest, r.notes, r.url);
                } else if (manual) {
                    new AlertDialog.Builder(activity)
                            .setTitle(r.reached ? "You're up to date" : "Could not check")
                            .setMessage(r.reached
                                    ? "You have the latest version (v" + currentVersion + ")."
                                    : "volvisionmods.com could not be reached. Check your internet connection and try again.")
                            .setPositiveButton("OK", null)
                            .setNeutralButton("Open website", (d, w) -> open(activity, STORE_URL))
                            .show();
                }
            });
        }, "update-check").start();
    }

    private static void showDialog(final Activity activity, String newVersion, String whatsNew, final String redirectUrl) {
        if (activity == null || activity.isFinishing()) return;
        new AlertDialog.Builder(activity)
                .setTitle("Update Available (v" + newVersion + ")")
                .setMessage(whatsNew + "\n\nTap Update Now to open the game page.")
                .setPositiveButton("Update Now", (dialog, which) -> open(activity, redirectUrl))
                .setNegativeButton("Later", null)
                .setCancelable(true)
                .show();
    }

    static void open(Context c, String url) {
        try {
            Intent i = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
            if (!(c instanceof Activity)) i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            c.startActivity(i);
        } catch (RuntimeException e) {
            Toast.makeText(c, "No browser found: " + url, Toast.LENGTH_LONG).show();
        }
    }

    /** The installed version name (build.gradle versionName, e.g. 1.0.1). */
    static String versionName(Context c) {
        try {
            String v = c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName;
            return v == null ? "0" : v;
        } catch (Exception e) {
            return "0";
        }
    }

    static boolean isNewerVersion(String remote, String local) {
        String[] r = remote.split("[.-]");
        String[] l = local.split("[.-]");
        int max = Math.max(r.length, l.length);
        for (int i = 0; i < max; i++) {
            int rv = i < r.length ? parseIntSafe(r[i]) : 0;
            int lv = i < l.length ? parseIntSafe(l[i]) : 0;
            if (rv > lv) return true;
            if (rv < lv) return false;
        }
        return false;
    }

    private static int parseIntSafe(String s) {
        try { return Integer.parseInt(s.replaceAll("\\D+", "")); } catch (Exception e) { return 0; }
    }
}
