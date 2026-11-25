package com.saavan.ttt2;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;

/**
 * Launcher and installer. The player picks their own Tekken Tag Tournament 2 disc image and the
 * Title Update 3 file with Android's file picker; libmain.so builds the game folder from them
 * (android_install.cpp: the PC installer's steps). Then PLAY starts GameActivity.
 * Game folder, saves and logs: Android/data/com.saavan.ttt2/files.
 */
public class LauncherActivity extends Activity {
    private static final int PICK_ISO = 1, PICK_TU = 2;

    private SharedPreferences prefs;
    private TextView isoText, tuText, status, info;
    private Button isoButton, tuButton, installButton, playButton, cancelButton;
    private ProgressBar progress;
    private volatile boolean cancel;
    private boolean installing;

    static String dataRoot(Context c) {
        File f = c.getExternalFilesDir(null);
        if (f == null) f = c.getFilesDir();
        return f.getAbsolutePath();
    }

    private boolean installed() {
        return new File(dataRoot(this), "game/tekken.img").exists() && new File(dataRoot(this), "game/.ttt2-installed").exists();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences("ttt2", MODE_PRIVATE);
        setContentView(buildUi());
        refresh();
    }

    // ---------------------------------------------------------------- UI
    private Button button(String text, int color) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(16);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(color);
        bg.setCornerRadius(dp(10));
        b.setBackground(bg);
        b.setPadding(dp(16), dp(10), dp(16), dp(10));
        return b;
    }

    private TextView label(String text, float size, int color) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(size);
        t.setTextColor(color);
        return t;
    }

    private int dp(float v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    private LinearLayout.LayoutParams lp(int top) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        p.topMargin = dp(top);
        return p;
    }

    private View buildUi() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(28), dp(20), dp(28), dp(20));
        col.setBackgroundColor(Color.rgb(14, 14, 22));

        TextView title = label("TEKKEN TAG TOURNAMENT 2", 26, Color.rgb(255, 140, 40));
        title.setTypeface(Typeface.DEFAULT_BOLD);
        col.addView(title);
        col.addView(label("Native Android port (static recompilation). Uses your own disc image and Title Update 3.", 13, Color.LTGRAY), lp(4));

        LinearLayout row1 = new LinearLayout(this);
        row1.setOrientation(LinearLayout.HORIZONTAL);
        row1.setGravity(Gravity.CENTER_VERTICAL);
        isoButton = button("1. Disc image (.iso)", Color.rgb(60, 60, 90));
        isoButton.setOnClickListener(v -> pick(PICK_ISO));
        row1.addView(isoButton);
        isoText = label("", 13, Color.WHITE);
        isoText.setPadding(dp(14), 0, 0, 0);
        row1.addView(isoText);
        col.addView(row1, lp(16));

        LinearLayout row2 = new LinearLayout(this);
        row2.setOrientation(LinearLayout.HORIZONTAL);
        row2.setGravity(Gravity.CENTER_VERTICAL);
        tuButton = button("2. Title Update 3 (TU_...O2)", Color.rgb(60, 60, 90));
        tuButton.setOnClickListener(v -> pick(PICK_TU));
        row2.addView(tuButton);
        tuText = label("", 13, Color.WHITE);
        tuText.setPadding(dp(14), 0, 0, 0);
        row2.addView(tuText);
        col.addView(row2, lp(8));

        LinearLayout row3 = new LinearLayout(this);
        row3.setOrientation(LinearLayout.HORIZONTAL);
        installButton = button("3. Install", Color.rgb(40, 110, 200));
        installButton.setOnClickListener(v -> startInstall());
        row3.addView(installButton);
        cancelButton = button("Cancel", Color.rgb(120, 40, 40));
        cancelButton.setOnClickListener(v -> cancel = true);
        LinearLayout.LayoutParams cp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        cp.leftMargin = dp(10);
        row3.addView(cancelButton, cp);
        col.addView(row3, lp(12));

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(100);
        col.addView(progress, lp(10));
        status = label("", 14, Color.WHITE);
        col.addView(status, lp(4));

        LinearLayout row4 = new LinearLayout(this);
        row4.setOrientation(LinearLayout.HORIZONTAL);
        row4.setGravity(Gravity.CENTER_VERTICAL);
        playButton = button("  PLAY  ", Color.rgb(230, 100, 20));
        playButton.setTextSize(22);
        playButton.setOnClickListener(v -> play());
        row4.addView(playButton);
        CheckBox touch = new CheckBox(this);
        touch.setText("On-screen controls");
        touch.setTextColor(Color.WHITE);
        touch.setChecked(prefs.getBoolean("touch", true));
        touch.setOnCheckedChangeListener((b, on) -> prefs.edit().putBoolean("touch", on).apply());
        LinearLayout.LayoutParams tp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        tp.leftMargin = dp(18);
        row4.addView(touch, tp);
        Button log = button("Last log", Color.rgb(60, 60, 60));
        log.setOnClickListener(v -> showLog());
        LinearLayout.LayoutParams lpp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lpp.leftMargin = dp(18);
        row4.addView(log, lpp);
        col.addView(row4, lp(18));

        info = label("", 12, Color.GRAY);
        col.addView(info, lp(14));

        ScrollView sv = new ScrollView(this);
        sv.addView(col);
        return sv;
    }

    private void refresh() {
        isoText.setText(nameOf(prefs.getString("iso", null), "not chosen"));
        tuText.setText(nameOf(prefs.getString("tu", null), "not chosen"));
        boolean ok = installed();
        playButton.setEnabled(ok && !installing);
        playButton.setAlpha(ok && !installing ? 1f : 0.4f);
        installButton.setEnabled(!installing && prefs.getString("iso", null) != null && prefs.getString("tu", null) != null);
        installButton.setAlpha(installButton.isEnabled() ? 1f : 0.4f);
        installButton.setText(ok ? "3. Reinstall" : "3. Install");
        isoButton.setEnabled(!installing);
        tuButton.setEnabled(!installing);
        cancelButton.setVisibility(installing ? View.VISIBLE : View.GONE);
        if (!installing && status.getText().length() == 0)
            status.setText(ok ? "Game installed. Press PLAY." : "Choose your disc image and Title Update 3, then Install (about 10 GB free space needed).");
        info.setText("Game folder, saves and logs: " + dataRoot(this) + "\nLog: TTT2Recomp/log.txt   DXVK logs: TTT2Recomp/*.log");
    }

    private String nameOf(String uri, String fallback) {
        if (uri == null) return fallback;
        try (Cursor c = getContentResolver().query(Uri.parse(uri), new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) {}
        return Uri.parse(uri).getLastPathSegment();
    }

    private void pick(int code) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, code);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        try { getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); } catch (Exception ignored) {}
        prefs.edit().putString(request == PICK_ISO ? "iso" : "tu", uri.toString()).apply();
        status.setText("");
        refresh();
    }

    // ---------------------------------------------------------------- install
    private void startInstall() {
        installing = true;
        cancel = false;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        refresh();
        new Thread(() -> {
            String error;
            try (ParcelFileDescriptor iso = getContentResolver().openFileDescriptor(Uri.parse(prefs.getString("iso", "")), "r");
                 ParcelFileDescriptor tu = getContentResolver().openFileDescriptor(Uri.parse(prefs.getString("tu", "")), "r")) {
                if (iso == null || tu == null) throw new Exception("Could not open the chosen files.");
                error = Native.install("/proc/self/fd/" + iso.getFd(), "/proc/self/fd/" + tu.getFd(), dataRoot(this), new Native.Listener() {
                    @Override public void progress(int percent, String text) {
                        runOnUiThread(() -> { progress.setProgress(percent); status.setText(text); });
                    }
                    @Override public boolean cancelled() { return cancel; }
                });
            } catch (Throwable t) {
                error = t.toString();
            }
            final String err = error;
            runOnUiThread(() -> {
                installing = false;
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                if (err == null) { progress.setProgress(100); status.setText("Installed. Press PLAY."); }
                else status.setText("Install failed: " + err);
                refresh();
            });
        }, "ttt2-install").start();
    }

    private void play() {
        startActivity(new Intent(this, GameActivity.class));
    }

    private void showLog() {
        File f = new File(dataRoot(this), "TTT2Recomp/log.txt");
        String text;
        try (InputStream in = new FileInputStream(f)) {
            byte[] b = new byte[(int) Math.min(f.length(), 200_000)];
            long skip = f.length() - b.length;
            if (skip > 0) in.skip(skip);
            int n = in.read(b);
            text = new String(b, 0, Math.max(n, 0), "UTF-8");
        } catch (Exception e) {
            text = "No log yet (" + f + ")";
        }
        final String all = text;
        TextView tv = label(all, 11, Color.WHITE);
        tv.setTypeface(Typeface.MONOSPACE);
        tv.setTextIsSelectable(true);
        tv.setPadding(dp(10), dp(10), dp(10), dp(10));
        ScrollView sv = new ScrollView(this);
        sv.addView(tv);
        sv.post(() -> sv.fullScroll(View.FOCUS_DOWN));
        new AlertDialog.Builder(this).setTitle("log.txt (end)").setView(sv)
            .setPositiveButton("Copy", (d, w) -> ((ClipboardManager) getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("ttt2 log", all)))
            .setNegativeButton("Close", null).show();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }
}
