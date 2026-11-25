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
        return a.toArray(new String[0]);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (mLayout != null && getSharedPreferences("ttt2", MODE_PRIVATE).getBoolean("touch", true)) {
            overlay = new TouchOverlay(this);
            mLayout.addView(overlay, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
        hideSystemBars();
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
