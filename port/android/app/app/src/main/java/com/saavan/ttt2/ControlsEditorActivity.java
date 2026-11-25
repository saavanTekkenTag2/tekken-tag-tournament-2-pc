package com.saavan.ttt2;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

/**
 * r23: the touch controls' layout editor (OPTIONS > TOOLS > EDIT TOUCH CONTROLS, and OPTIONS > CONTROLS). Drag a group
 * (stick, buttons, shoulders, start/back) to move it; the bar sets the selected group's size and opacity. Saved in the
 * app's preferences, read by TouchOverlay when the game starts.
 */
public class ControlsEditorActivity extends Activity {
    private TouchOverlay pad;
    private TextView groupName;
    private SeekBar size, opacity;
    private boolean updating;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        LauncherActivity.useFullScreen(this);
        float d = getResources().getDisplayMetrics().density;
        FrameLayout root = new FrameLayout(this);
        root.setBackground(new GradientDrawable(GradientDrawable.Orientation.TL_BR,
            new int[] { Color.rgb(30, 26, 44), Color.rgb(12, 10, 18), Color.rgb(46, 10, 16) }));
        TextView hint = new TextView(this);
        hint.setText("Drag a group to move it. Pick a group, then set its size and opacity.");
        hint.setTextColor(Color.argb(150, 220, 224, 235));
        hint.setTextSize(13);
        FrameLayout.LayoutParams hp = new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER);
        root.addView(hint, hp);

        pad = new TouchOverlay(this);
        pad.editMode = true;
        pad.loadLayout();
        root.addView(pad, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout bar = new LinearLayout(this);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(Math.round(14 * d), Math.round(6 * d), Math.round(14 * d), Math.round(6 * d));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.argb(225, 10, 8, 14));
        bg.setStroke(Math.round(d), Color.argb(80, 220, 224, 235));
        bg.setCornerRadius(14 * d);
        bar.setBackground(bg);
        groupName = label("BUTTONS", 15, Color.WHITE);
        groupName.setMinWidth(Math.round(120 * d));
        bar.addView(groupName);
        bar.addView(label("SIZE", 12, Color.rgb(150, 154, 170)));
        size = new SeekBar(this);
        size.setMax(100);                    // 50% .. 150%
        bar.addView(size, new LinearLayout.LayoutParams(Math.round(150 * d), -2));
        bar.addView(label("OPACITY", 12, Color.rgb(150, 154, 170)));
        opacity = new SeekBar(this);
        opacity.setMax(85);                  // 15% .. 100%
        bar.addView(opacity, new LinearLayout.LayoutParams(Math.round(150 * d), -2));
        bar.addView(button("RESET", Color.rgb(60, 60, 72), v -> { pad.resetLayout(); showGroup(pad.selected); }));
        bar.addView(button("DONE", Color.rgb(214, 24, 44), v -> { pad.saveLayout(); finish(); }));
        FrameLayout.LayoutParams bp = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        bp.topMargin = Math.round(56 * d);
        root.addView(bar, bp);

        pad.selectionListener = this::showGroup;
        size.setOnSeekBarChangeListener(new Seek() { @Override public void onProgressChanged(SeekBar s, int v, boolean user) {
            if (updating) return; pad.gs[pad.selected] = 0.5f + v / 100f; pad.relayout(); } });
        opacity.setOnSeekBarChangeListener(new Seek() { @Override public void onProgressChanged(SeekBar s, int v, boolean user) {
            if (updating) return; pad.ga[pad.selected] = 0.15f + v / 100f; pad.invalidate(); } });
        setContentView(root);
        showGroup(pad.selected);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }

    private void showGroup(int g) {
        updating = true;
        groupName.setText(TouchOverlay.GROUP_NAMES[g]);
        size.setProgress(Math.round((pad.gs[g] - 0.5f) * 100));
        opacity.setProgress(Math.round((pad.ga[g] - 0.15f) * 100));
        updating = false;
        pad.invalidate();
    }

    @Override
    public void onBackPressed() { pad.saveLayout(); super.onBackPressed(); }

    private TextView label(String s, float size, int color) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(size);
        t.setTextColor(color);
        t.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        t.setPadding(Math.round(8 * getResources().getDisplayMetrics().density), 0, 0, 0);
        return t;
    }

    private TextView button(String s, int color, View.OnClickListener l) {
        float d = getResources().getDisplayMetrics().density;
        TextView t = label(s, 13, Color.WHITE);
        t.setPadding(Math.round(16 * d), Math.round(8 * d), Math.round(16 * d), Math.round(8 * d));
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(10 * d);
        t.setBackground(g);
        t.setOnClickListener(l);
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-2, -2);
        p.leftMargin = Math.round(10 * d);
        t.setLayoutParams(p);
        return t;
    }

    private abstract static class Seek implements SeekBar.OnSeekBarChangeListener {
        @Override public void onStartTrackingTouch(SeekBar s) {}
        @Override public void onStopTrackingTouch(SeekBar s) {}
    }
}
