package com.saavan.ttt2;

import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

/** The game: SDL2 runs libmain.so's SDL_main; the touch controls sit on top of its surface. */
public class GameActivity extends SDLActivity {
    private TouchOverlay overlay;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        android.content.SharedPreferences p = getSharedPreferences("ttt2", MODE_PRIVATE);
        java.util.ArrayList<String> a = new java.util.ArrayList<>();
        a.add("--root"); a.add(LauncherActivity.dataRoot(this));
        if (p.getBoolean("fbdump", false)) a.add("--fbdump");
        if (p.getBoolean("cpu", false)) a.add("--cpu");
        a.addAll(Settings.envArgs(this));     // r22: launcher OPTIONS (runtime switches)
        return a.toArray(new String[0]);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        LauncherActivity.useFullScreen(this);     // r23: the picture under the camera cut-out too
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (mLayout != null && getSharedPreferences("ttt2", MODE_PRIVATE).getBoolean("touch", true)) {
            overlay = new TouchOverlay(this);
            mLayout.addView(overlay, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
        hideSystemBars();
        applyPerformanceMode();
    }

    /** r22: steady clocks (sustained performance mode) and a 60 Hz display while the game runs (OPTIONS -> PERFORMANCE). */
    private void applyPerformanceMode() {
        android.content.SharedPreferences p = getSharedPreferences("ttt2", MODE_PRIVATE);
        if (p.getBoolean("sustained", true)) {
            try {
                android.os.PowerManager pm = (android.os.PowerManager) getSystemService(POWER_SERVICE);
                if (pm != null && pm.isSustainedPerformanceModeSupported()) getWindow().setSustainedPerformanceMode(true);
            } catch (Throwable ignored) {}
        }
        if (p.getBoolean("fps60", true)) {
            try {
                // The display mode closest to 60 Hz at the current size (90/120 Hz screens pace 60 fps unevenly).
                android.view.Display d = getWindowManager().getDefaultDisplay();
                android.view.Display.Mode cur = d.getMode(), best = null;
                for (android.view.Display.Mode m : d.getSupportedModes()) {
                    if (m.getPhysicalWidth() != cur.getPhysicalWidth() || m.getPhysicalHeight() != cur.getPhysicalHeight()) continue;
                    if (best == null || Math.abs(m.getRefreshRate() - 60f) < Math.abs(best.getRefreshRate() - 60f)) best = m;
                }
                if (best != null && Math.abs(best.getRefreshRate() - 60f) < 1f) {
                    WindowManager.LayoutParams lp = getWindow().getAttributes();
                    lp.preferredDisplayModeId = best.getModeId();
                    getWindow().setAttributes(lp);
                }
            } catch (Throwable ignored) {}
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }
}
