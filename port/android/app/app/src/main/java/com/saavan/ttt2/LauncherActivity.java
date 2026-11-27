package com.saavan.ttt2;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ObjectAnimator;
import android.animation.ValueAnimator;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.text.InputType;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewOutlineProvider;
import android.view.WindowManager;
import android.view.animation.DecelerateInterpolator;
import android.view.animation.LinearInterpolator;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Launcher (r22 redesign): an animated home screen over the game's art - PLAY, SELECT ISO, SELECT TITLE UPDATE,
 * OPTIONS (every setting: Settings.java), CREDITS (Credits.java). The player picks their own disc image and
 * Title Update 3 with Android's file picker; libmain.so builds the game folder from them (android_install.cpp).
 * Motion: opacity and transform only (no live blur), 0.2-0.4 s transitions; OPTIONS -> LAUNCHER -> REMOVE
 * ANIMATIONS (or the system's "Remove animations") shows everything at once.
 * Game folder, saves and logs: Android/data/com.saavan.ttt2/files.
 */
public class LauncherActivity extends Activity {
    private static final int PICK_ISO = 1, PICK_TU = 2, PICK_FOLDER = 3, PICK_DLC_DIR = 4, PICK_DLC_FILES = 5;

    // The game's colours: Tekken red, steel / silver, smoke violet, ink black.
    static final int RED = Color.rgb(226, 22, 34), RED_DARK = Color.rgb(120, 8, 16), SILVER = Color.rgb(222, 224, 232),
        STEEL = Color.rgb(150, 154, 170), INK = Color.rgb(9, 8, 13), SMOKE = Color.rgb(64, 58, 88), BLUE = Color.rgb(70, 160, 255);

    private SharedPreferences prefs;
    private FrameLayout root, pages;
    private ImageView bg;
    private final List<View> streaks = new ArrayList<>();
    private final List<Animator> loops = new ArrayList<>();
    private View home, current;
    private TextView status, cornerVersion, cornerArrow;
    private View playBadge;
    private ProgressBar progress;
    private MenuItem playItem, isoItem, tuItem;
    private final List<MenuItem> menuItems = new ArrayList<>();
    private View installPanel;
    private TextView installText, installTitle;
    private ProgressBar installBar;
    private volatile boolean cancel;
    private boolean installing, entered;
    private Typeface bold, semi, regular;

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
        useFullScreen(this);
        try { bold = getResources().getFont(R.font.oswald_bold); semi = getResources().getFont(R.font.oswald_semibold); regular = getResources().getFont(R.font.oswald_regular); }
        catch (Exception e) { bold = Typeface.DEFAULT_BOLD; semi = Typeface.DEFAULT_BOLD; regular = Typeface.DEFAULT; }
        Settings.enforce(this);                 // r28: locked settings back to their tested values
        setContentView(buildRoot());
        hideSystemBars();
        refresh();
        // r28: updates from volvisionmods.com - a popup now if there is a newer version, a notification later.
        if (savedInstanceState == null) VolvisionUpdateChannel.checkForUpdates(this, VolvisionUpdateChannel.versionName(this));
        UpdateJob.createChannel(this);
        UpdateJob.schedule(this);
        if (android.os.Build.VERSION.SDK_INT >= 33 && !prefs.getBoolean("asked_notifications", false)
                && checkSelfPermission("android.permission.POST_NOTIFICATIONS") != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            prefs.edit().putBoolean("asked_notifications", true).apply();
            requestPermissions(new String[] { "android.permission.POST_NOTIFICATIONS" }, 77);
        }
    }

    /** r23: draw under the camera cut-out too (it left a black band down one side in landscape). Option: "full_screen". */
    static void useFullScreen(Activity a) {
        if (!a.getSharedPreferences("ttt2", MODE_PRIVATE).getBoolean("full_screen", true)) return;
        WindowManager.LayoutParams lp = a.getWindow().getAttributes();
        lp.layoutInDisplayCutoutMode = android.os.Build.VERSION.SDK_INT >= 30
            ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
            : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        a.getWindow().setAttributes(lp);
    }

    private boolean noAnim() { return Settings.animationsOff(this); }
    private int dp(float v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    // ================================================================ building blocks
    private TextView text(String s, float size, int color, Typeface tf) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(size);
        t.setTextColor(color);
        t.setTypeface(tf);
        t.setIncludeFontPadding(false);
        return t;
    }

    private static FrameLayout.LayoutParams flp(int w, int h, int gravity) {
        FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(w, h);
        p.gravity = gravity;
        return p;
    }

    private LinearLayout.LayoutParams llp(int w, int h, int top) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(w, h);
        p.topMargin = dp(top);
        return p;
    }

    /** A slanted plate (the game's menu look): dark glass, a red edge on the left, a silver hairline. */
    static final class Plate extends android.graphics.drawable.Drawable {
        final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), edge = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG);
        final float slant;
        Plate(float slant, int fillColor, int edgeColor) {
            this.slant = slant;
            fill.setColor(fillColor);
            edge.setColor(edgeColor);
            line.setStyle(Paint.Style.STROKE);
            line.setStrokeWidth(1.5f);
            line.setColor(Color.argb(70, 220, 224, 235));
        }
        @Override public void draw(Canvas c) {
            float w = getBounds().width(), h = getBounds().height();
            Path p = new Path();
            p.moveTo(slant, 0); p.lineTo(w, 0); p.lineTo(w - slant, h); p.lineTo(0, h); p.close();
            c.drawPath(p, fill);
            c.drawPath(p, line);
            Path e = new Path();
            float ew = slant * 0.55f + 6;
            e.moveTo(slant, 0); e.lineTo(slant + ew, 0); e.lineTo(ew, h); e.lineTo(0, h); e.close();
            c.drawPath(e, edge);
        }
        @Override public void setAlpha(int a) {}
        @Override public void setColorFilter(android.graphics.ColorFilter cf) {}
        @Override public int getOpacity() { return android.graphics.PixelFormat.TRANSLUCENT; }
    }

    /** A home-menu entry: the plate, a bright plate on top shown on press / focus, the title and a small subtitle. */
    final class MenuItem extends FrameLayout {
        final View glow;
        final TextView title, sub;
        MenuItem(String label, Runnable action) {
            super(LauncherActivity.this);
            setFocusable(true);
            setClickable(true);
            setBackground(new Plate(dp(16), Color.argb(170, 12, 10, 18), RED));
            glow = new View(getContext());
            glow.setBackground(new Plate(dp(16), Color.argb(150, 160, 14, 26), Color.rgb(255, 70, 70)));
            glow.setAlpha(0f);
            addView(glow, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
            LinearLayout col = new LinearLayout(getContext());
            col.setOrientation(LinearLayout.VERTICAL);
            col.setGravity(Gravity.CENTER_VERTICAL);
            col.setPadding(dp(34), 0, dp(22), 0);
            title = text(label, 19, SILVER, bold);
            title.setLetterSpacing(0.06f);
            col.addView(title);
            sub = text("", 10.5f, STEEL, regular);
            sub.setLetterSpacing(0.04f);
            sub.setVisibility(GONE);
            col.addView(sub);
            addView(col, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
            setOnClickListener(v -> action.run());
            setOnFocusChangeListener((v, f) -> highlight(f));
            setOnTouchListener((v, ev) -> {
                if (ev.getActionMasked() == MotionEvent.ACTION_DOWN) press(true);
                else if (ev.getActionMasked() == MotionEvent.ACTION_UP || ev.getActionMasked() == MotionEvent.ACTION_CANCEL) press(false);
                return false;
            });
        }
        void setSub(String s) { sub.setText(s); sub.setVisibility(s == null || s.isEmpty() ? GONE : VISIBLE); }
        void press(boolean down) {
            if (noAnim()) { glow.setAlpha(down || isFocused() ? 1f : 0f); return; }
            animate().scaleX(down ? 0.96f : 1f).scaleY(down ? 0.96f : 1f).setDuration(down ? 120 : 220).setInterpolator(new DecelerateInterpolator()).start();
            glow.animate().alpha(down || isFocused() ? 1f : 0f).setDuration(down ? 120 : 260).start();
        }
        void highlight(boolean on) {
            if (noAnim()) { glow.setAlpha(on ? 1f : 0f); setTranslationX(on ? dp(10) : 0); return; }
            glow.animate().alpha(on ? 1f : 0f).setDuration(220).start();
            animate().translationX(on ? dp(10) : 0).setDuration(240).setInterpolator(new DecelerateInterpolator()).start();
        }
    }

    /** An original play badge: a red disc, a white triangle and a soft ring (not any brand's logo). */
    final class PlayBadge extends View {
        final Paint disc = new Paint(Paint.ANTI_ALIAS_FLAG), tri = new Paint(Paint.ANTI_ALIAS_FLAG), ring = new Paint(Paint.ANTI_ALIAS_FLAG);
        PlayBadge() {
            super(LauncherActivity.this);
            disc.setColor(RED);
            tri.setColor(Color.WHITE);
            ring.setStyle(Paint.Style.STROKE);
            ring.setStrokeWidth(dp(1.5f));
            ring.setColor(Color.argb(150, 255, 120, 120));
        }
        @Override protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(), r = Math.min(w, h) / 2f - dp(2);
            c.drawCircle(w / 2, h / 2, r + dp(1), ring);
            c.drawCircle(w / 2, h / 2, r - dp(1), disc);
            Path p = new Path();
            float s = r * 0.42f;
            p.moveTo(w / 2 - s * 0.7f, h / 2 - s);
            p.lineTo(w / 2 + s, h / 2);
            p.lineTo(w / 2 - s * 0.7f, h / 2 + s);
            p.close();
            c.drawPath(p, tri);
        }
    }

    // ================================================================ root and home
    private View buildRoot() {
        root = new FrameLayout(this);
        root.setBackgroundColor(INK);

        bg = new ImageView(this);
        bg.setImageResource(R.drawable.home_bg);
        // r23: filled edge to edge and anchored at the top (the logo is there): CENTER_CROP cut the logo off on wide
        // phones. The motion's zoom pivots on the top edge.
        bg.setScaleType(ImageView.ScaleType.MATRIX);
        bg.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> fitBackground());
        root.addView(bg, flp(-1, -1, Gravity.FILL));

        // Static scrims (no live blur): darker left half for the menu, darker bottom edge for the footer.
        View left = new View(this);
        left.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
            new int[] { Color.argb(215, 6, 5, 10), Color.argb(130, 6, 5, 10), Color.argb(0, 6, 5, 10) }));
        root.addView(left, flp(-1, -1, Gravity.FILL));
        View bottom = new View(this);
        bottom.setBackground(new GradientDrawable(GradientDrawable.Orientation.BOTTOM_TOP,
            new int[] { Color.argb(220, 6, 5, 10), Color.argb(0, 6, 5, 10) }));
        root.addView(bottom, flp(-1, dp(90), Gravity.BOTTOM));

        // Light streaks sweeping across (transform + opacity only).
        int[][] streakColors = { { RED, 0 }, { SILVER, 0 }, { BLUE, 0 } };
        for (int i = 0; i < 3; i++) {
            View s = new View(this);
            GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] { Color.argb(0, 255, 255, 255), withAlpha(streakColors[i][0], 200), Color.argb(0, 255, 255, 255) });
            s.setBackground(g);
            s.setRotation(-14f);
            s.setAlpha(0f);
            root.addView(s, flp(dp(260), dp(i == 1 ? 1.5f : 2.5f), Gravity.TOP | Gravity.START));
            streaks.add(s);
        }

        pages = new FrameLayout(this);
        root.addView(pages, flp(-1, -1, Gravity.FILL));
        // The art goes under the camera cut-out; the menus keep clear of it.
        root.setOnApplyWindowInsetsListener((v, in) -> {
            android.view.DisplayCutout c = in.getDisplayCutout();
            int l = c != null ? c.getSafeInsetLeft() : 0, r = c != null ? c.getSafeInsetRight() : 0;
            pages.setPadding(l, 0, r, 0);
            if (installPanel != null) installPanel.setPadding(l, 0, r, 0);
            return in;
        });
        home = buildHome();
        pages.addView(home, flp(-1, -1, Gravity.FILL));
        current = home;

        installPanel = buildInstallPanel();
        installPanel.setVisibility(View.GONE);
        root.addView(installPanel, flp(-1, -1, Gravity.FILL));
        return root;
    }

    private static final float BG_ZOOM = 1.035f;

    private void fitBackground() {
        android.graphics.drawable.Drawable d = bg.getDrawable();
        int vw = bg.getWidth(), vh = bg.getHeight();
        if (d == null || vw == 0 || vh == 0) return;
        float iw = d.getIntrinsicWidth(), ih = d.getIntrinsicHeight();
        float scale = Math.max(vw / iw, vh / ih);
        android.graphics.Matrix m = new android.graphics.Matrix();
        m.setScale(scale, scale);
        m.postTranslate((vw - iw * scale) / 2f, 0);
        bg.setImageMatrix(m);
        bg.setPivotX(vw / 2f);
        bg.setPivotY(0);
    }

    private static int withAlpha(int c, int a) { return Color.argb(a, Color.red(c), Color.green(c), Color.blue(c)); }

    private View buildHome() {
        FrameLayout f = new FrameLayout(this);
        // The art's logo is in its top-left third; the menu sits under it.
        LinearLayout menu = new LinearLayout(this);
        menu.setOrientation(LinearLayout.VERTICAL);
        playItem = new MenuItem("PLAY", this::play);
        isoItem = new MenuItem("SELECT GAME", this::chooseGameSource);
        tuItem = new MenuItem("SELECT TITLE UPDATE", () -> pick(PICK_TU));
        MenuItem options = new MenuItem("OPTIONS", () -> showPage(buildOptions()));
        MenuItem updates = new MenuItem("CHECK FOR UPDATES", () -> VolvisionUpdateChannel.checkNow(this, VolvisionUpdateChannel.versionName(this)));
        MenuItem credits = new MenuItem("CREDITS", () -> showPage(buildCredits()));
        menuItems.clear();
        menuItems.add(playItem); menuItems.add(isoItem); menuItems.add(tuItem); menuItems.add(options); menuItems.add(updates); menuItems.add(credits);
        playItem.title.setTextSize(23);
        // Six entries: a little shorter on short screens so the last one never runs under the footer.
        final boolean shortScreen = getResources().getDisplayMetrics().heightPixels / getResources().getDisplayMetrics().density < 380;
        for (int i = 0; i < menuItems.size(); i++)
            menu.addView(menuItems.get(i), llp(dp(300), dp(i == 0 ? (shortScreen ? 40 : 44) : (shortScreen ? 32 : 37)), i == 0 ? 0 : (shortScreen ? 4 : 6)));
        FrameLayout.LayoutParams mp = flp(-2, -2, Gravity.START | Gravity.BOTTOM);
        mp.leftMargin = dp(26);
        mp.bottomMargin = dp(40);
        f.addView(menu, mp);

        // Footer: install status on the left, the port's author on the right.
        LinearLayout foot = new LinearLayout(this);
        foot.setOrientation(LinearLayout.VERTICAL);
        status = text("", 11, STEEL, regular);
        foot.addView(status);
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(100);
        progress.setVisibility(View.GONE);
        foot.addView(progress, llp(dp(300), dp(4), 3));
        FrameLayout.LayoutParams fp = flp(-2, -2, Gravity.START | Gravity.BOTTOM);
        fp.leftMargin = dp(30);
        fp.bottomMargin = dp(14);
        f.addView(foot, fp);

        // r26: "PORTED BY CATONIX" with a pulsing play badge and an arrow - a tap opens the YouTube channel.
        LinearLayout corner = new LinearLayout(this);
        corner.setOrientation(LinearLayout.VERTICAL);
        corner.setGravity(Gravity.END);
        corner.setPadding(dp(10), dp(6), dp(6), dp(4));
        TextView by = text("PORTED BY", 9.5f, STEEL, semi);
        by.setLetterSpacing(0.25f);
        corner.addView(by);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        TextView name = text(Credits.PORT_BY, 16, SILVER, bold);
        name.setLetterSpacing(0.08f);
        row.addView(name);
        playBadge = new PlayBadge();
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(dp(26), dp(26));
        bp.leftMargin = dp(8);
        row.addView(playBadge, bp);
        cornerArrow = text("\u203A", 22, Color.WHITE, bold);
        cornerArrow.setPadding(dp(6), 0, 0, dp(2));
        row.addView(cornerArrow);
        corner.addView(row);
        TextView yt = text("WATCH ON YOUTUBE", 8.5f, RED, semi);
        yt.setLetterSpacing(0.2f);
        corner.addView(yt);
        cornerVersion = text(version(), 9f, STEEL, regular);
        corner.addView(cornerVersion);
        corner.setFocusable(true);
        corner.setClickable(true);
        corner.setOnClickListener(v -> open(Credits.BUTTONS[0][1]));
        tapFeedback(corner);
        FrameLayout.LayoutParams cp = flp(-2, -2, Gravity.END | Gravity.BOTTOM);
        cp.rightMargin = dp(24);
        cp.bottomMargin = dp(14);
        f.addView(corner, cp);
        f.setTag(corner);
        return f;
    }

    private String version() {
        try { return "v" + getPackageManager().getPackageInfo(getPackageName(), 0).versionName; }
        catch (Exception e) { return ""; }
    }

    // ================================================================ motion
    @Override
    protected void onResume() {
        super.onResume();
        refresh();
        if (!entered) { entered = true; root.post(this::enter); }
        startLoops();
    }

    @Override
    protected void onPause() {
        super.onPause();
        stopLoops();
    }

    /** Entrance: the art fades and settles, the menu slides in one entry after another, the corner fades in. */
    private void enter() {
        View corner = (View) home.getTag();
        if (noAnim()) {
            bg.setAlpha(1f); bg.setScaleX(BG_ZOOM); bg.setScaleY(BG_ZOOM);
            for (MenuItem m : menuItems) { m.setAlpha(1f); m.setTranslationX(0); }
            corner.setAlpha(1f);
            playItem.requestFocus();
            return;
        }
        bg.setAlpha(0f); bg.setScaleX(1.10f); bg.setScaleY(1.10f);
        bg.animate().alpha(1f).scaleX(BG_ZOOM).scaleY(BG_ZOOM).setDuration(400).setInterpolator(new DecelerateInterpolator()).start();
        for (int i = 0; i < menuItems.size(); i++) {
            MenuItem m = menuItems.get(i);
            m.setAlpha(0f);
            m.setTranslationX(-dp(60));
            m.animate().alpha(1f).translationX(0).setStartDelay(160 + i * 60L).setDuration(320).setInterpolator(new DecelerateInterpolator(1.6f)).start();
        }
        corner.setAlpha(0f);
        corner.animate().alpha(1f).setStartDelay(420).setDuration(300).start();
        playItem.postDelayed(playItem::requestFocus, 500);
    }

    /** Background motion: a slow drift of the art and light streaks crossing now and then. */
    private void startLoops() {
        stopLoops();
        if (noAnim()) { bg.setScaleX(BG_ZOOM); bg.setScaleY(BG_ZOOM); bg.setTranslationX(0); bg.setTranslationY(0); return; }
        // Drift within the zoom's margin (1.75% each side), so no edge ever shows.
        float mx = Math.max(1f, root.getWidth() * (BG_ZOOM - 1f) / 2f * 0.9f);
        ObjectAnimator drift = ObjectAnimator.ofFloat(bg, View.TRANSLATION_X, -mx, mx);
        drift.setDuration(18000);
        drift.setRepeatCount(ValueAnimator.INFINITE);
        drift.setRepeatMode(ValueAnimator.REVERSE);
        drift.setInterpolator(new android.view.animation.AccelerateDecelerateInterpolator());
        drift.start();
        loops.add(drift);
        float my = Math.max(1f, root.getHeight() * (BG_ZOOM - 1f) * 0.8f);
        ObjectAnimator driftY = ObjectAnimator.ofFloat(bg, View.TRANSLATION_Y, 0, -my);
        driftY.setDuration(23000);
        driftY.setRepeatCount(ValueAnimator.INFINITE);
        driftY.setRepeatMode(ValueAnimator.REVERSE);
        driftY.start();
        loops.add(driftY);
        if (playBadge != null) {
            ObjectAnimator px = ObjectAnimator.ofFloat(playBadge, View.SCALE_X, 1f, 1.14f);
            ObjectAnimator py = ObjectAnimator.ofFloat(playBadge, View.SCALE_Y, 1f, 1.14f);
            for (ObjectAnimator a : new ObjectAnimator[] { px, py }) {
                a.setDuration(700);
                a.setRepeatCount(ValueAnimator.INFINITE);
                a.setRepeatMode(ValueAnimator.REVERSE);
                a.setInterpolator(new android.view.animation.AccelerateDecelerateInterpolator());
                a.start();
                loops.add(a);
            }
            ObjectAnimator ar = ObjectAnimator.ofFloat(cornerArrow, View.TRANSLATION_X, 0, dp(5));
            ar.setDuration(600);
            ar.setRepeatCount(ValueAnimator.INFINITE);
            ar.setRepeatMode(ValueAnimator.REVERSE);
            ar.start();
            loops.add(ar);
        }
        for (int i = 0; i < streaks.size(); i++) {
            final View s = streaks.get(i);
            final int idx = i;
            ValueAnimator a = ValueAnimator.ofFloat(0f, 1f);
            a.setDuration(2600 + i * 500L);
            a.setStartDelay(900 + i * 1700L);
            a.setRepeatCount(ValueAnimator.INFINITE);
            a.setInterpolator(new LinearInterpolator());
            a.addUpdateListener(an -> {
                float t = (float) an.getAnimatedValue();
                float w = root.getWidth(), h = root.getHeight();
                s.setTranslationX(-dp(300) + t * (w + dp(600)));
                s.setTranslationY(h * (0.18f + 0.27f * idx) - t * h * 0.12f);
                s.setAlpha((float) Math.sin(Math.PI * t) * (idx == 1 ? 0.55f : 0.8f));
            });
            a.start();
            loops.add(a);
        }
    }

    private void stopLoops() {
        for (Animator a : loops) a.cancel();
        loops.clear();
        for (View s : streaks) s.setAlpha(0f);
    }

    /** Page change: the old page slides out to the left and fades, the new one slides in from the right. */
    private void showPage(View next) {
        if (next == current) return;
        final View old = current;
        current = next;
        if (next.getParent() == null) pages.addView(next, flp(-1, -1, Gravity.FILL));
        if (noAnim()) {
            if (old != home) pages.removeView(old); else old.setVisibility(View.GONE);
            next.setVisibility(View.VISIBLE); next.setAlpha(1f); next.setTranslationX(0);
            next.requestFocus();
            return;
        }
        old.animate().alpha(0f).translationX(-dp(36)).setDuration(220).setInterpolator(new DecelerateInterpolator())
            .setListener(new AnimatorListenerAdapter() {
                @Override public void onAnimationEnd(Animator a) {
                    old.animate().setListener(null);
                    if (old != home) pages.removeView(old); else old.setVisibility(View.GONE);
                }
            }).start();
        next.setVisibility(View.VISIBLE);
        next.setAlpha(0f);
        next.setTranslationX(dp(36));
        next.animate().alpha(1f).translationX(0).setStartDelay(90).setDuration(300).setInterpolator(new DecelerateInterpolator(1.4f))
            .setListener(null).start();
        next.requestFocus();
    }

    private void goHome() {
        home.setAlpha(noAnim() ? 1f : home.getAlpha());
        showPage(home);
        refresh();
        playItem.requestFocus();
    }

    @Override
    public void onBackPressed() {
        if (installPanel.getVisibility() == View.VISIBLE) return;
        if (current != home) goHome();
        else super.onBackPressed();
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if ((keyCode == KeyEvent.KEYCODE_BUTTON_B) && current != home) { goHome(); return true; }
        return super.onKeyDown(keyCode, event);
    }

    // ================================================================ page frame
    /** A page: a back plate and a title at the top left, the content under it on a dark glass panel. */
    private FrameLayout page(String title, View content) {
        FrameLayout f = new FrameLayout(this);
        f.setClickable(true);
        View shade = new View(this);
        shade.setBackgroundColor(Color.argb(150, 6, 5, 10));
        f.addView(shade, flp(-1, -1, Gravity.FILL));

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(26), dp(16), dp(26), dp(12));
        LinearLayout head = new LinearLayout(this);
        head.setGravity(Gravity.CENTER_VERTICAL);
        TextView back = text("‹  BACK", 13, SILVER, semi);
        back.setLetterSpacing(0.12f);
        back.setPadding(dp(22), dp(7), dp(20), dp(7));
        back.setBackground(new Plate(dp(10), Color.argb(170, 12, 10, 18), RED));
        back.setFocusable(true);
        back.setOnClickListener(v -> goHome());
        tapFeedback(back);
        head.addView(back);
        TextView t = text(title, 26, SILVER, bold);
        t.setLetterSpacing(0.1f);
        t.setPadding(dp(18), 0, 0, 0);
        head.addView(t);
        View bar = new View(this);
        bar.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[] { RED, Color.argb(0, 226, 22, 34) }));
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(0, dp(2), 1f);
        bp.leftMargin = dp(16);
        head.addView(bar, bp);
        col.addView(head);
        LinearLayout.LayoutParams cp = new LinearLayout.LayoutParams(-1, 0, 1f);
        cp.topMargin = dp(10);
        col.addView(content, cp);
        f.addView(col, flp(-1, -1, Gravity.FILL));
        return f;
    }

    private void tapFeedback(View v) {
        v.setOnTouchListener((view, ev) -> {
            if (noAnim()) return false;
            if (ev.getActionMasked() == MotionEvent.ACTION_DOWN)
                view.animate().scaleX(0.95f).scaleY(0.95f).alpha(0.85f).setDuration(110).start();
            else if (ev.getActionMasked() == MotionEvent.ACTION_UP || ev.getActionMasked() == MotionEvent.ACTION_CANCEL)
                view.animate().scaleX(1f).scaleY(1f).alpha(1f).setDuration(220).start();
            return false;
        });
    }

    private GradientDrawable panel(int alpha) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(Color.argb(alpha, 14, 12, 20));
        g.setStroke(dp(1), Color.argb(60, 220, 224, 235));
        g.setCornerRadius(dp(4));
        return g;
    }

    // ================================================================ OPTIONS
    private String optionsGroup = "DISPLAY";

    private View buildOptions() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);

        // Group tabs.
        HorizontalScrollView tabsScroll = new HorizontalScrollView(this);
        tabsScroll.setHorizontalScrollBarEnabled(false);
        LinearLayout tabs = new LinearLayout(this);
        tabsScroll.addView(tabs);
        col.addView(tabsScroll);

        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(true);
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(dp(4), dp(6), dp(4), dp(20));
        sv.addView(list);
        LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(-1, 0, 1f);
        sp.topMargin = dp(8);
        col.addView(sv, sp);

        final List<TextView> tabViews = new ArrayList<>();
        String[] groups = new String[Settings.GROUPS.length + 1];
        System.arraycopy(Settings.GROUPS, 0, groups, 0, Settings.GROUPS.length);
        groups[groups.length - 1] = "TOOLS";
        if (!Settings.groupVisible(this, optionsGroup)) optionsGroup = "DISPLAY";
        for (String g : groups) {
            if (!Settings.groupVisible(this, g)) continue;
            TextView tab = text(g, 13, STEEL, semi);
            tab.setLetterSpacing(0.14f);
            tab.setPadding(dp(18), dp(8), dp(18), dp(8));
            tab.setFocusable(true);
            tab.setOnClickListener(v -> {
                optionsGroup = g;
                for (TextView tv : tabViews) styleTab(tv, tv.getText().toString().equals(optionsGroup));
                fillOptions(list);
                if (!noAnim()) {
                    list.setAlpha(0f); list.setTranslationY(dp(10));
                    list.animate().alpha(1f).translationY(0).setDuration(240).setInterpolator(new DecelerateInterpolator()).start();
                }
                sv.scrollTo(0, 0);
            });
            tapFeedback(tab);
            tabViews.add(tab);
            LinearLayout.LayoutParams tp = new LinearLayout.LayoutParams(-2, -2);
            tp.rightMargin = dp(6);
            tabs.addView(tab, tp);
        }
        for (TextView tv : tabViews) styleTab(tv, tv.getText().toString().equals(optionsGroup));
        fillOptions(list);
        return page("OPTIONS", col);
    }

    private void styleTab(TextView t, boolean on) {
        t.setTextColor(on ? Color.WHITE : STEEL);
        t.setBackground(new Plate(dp(9), on ? Color.argb(200, 150, 12, 24) : Color.argb(140, 12, 10, 18), on ? Color.rgb(255, 80, 80) : Color.argb(120, 226, 22, 34)));
    }

    private void fillOptions(LinearLayout list) {
        list.removeAllViews();
        if (optionsGroup.equals("TOOLS")) { fillTools(list); return; }
        final LinkedHashMap<String, LinkedHashMap<String, String>> ini = Settings.readIni(this);
        if (optionsGroup.equals("DISPLAY") || optionsGroup.equals("GAME") || optionsGroup.equals("ONLINE"))
            list.addView(note("These are the game's own settings (settings.ini) - the same as in the game's Options menu. They apply the next time the game starts."));
        else if (optionsGroup.equals("PERFORMANCE"))
            list.addView(note("For smooth play: Resolution 540p or 720p, Async pipelines ON, Async shaders ON, Big cores ON. "
                + "The first match on each stage builds its shaders once; later starts load them."));
        for (Settings.Opt o : Settings.ALL)
            if (o.group.equals(optionsGroup) && !Settings.locked(this, o)) list.addView(optionRow(o, ini), llp(-1, -2, 6));
        if (!Settings.devUnlocked(this) && optionsGroup.equals("DISPLAY"))
            list.addView(note("Performance settings are set for the smoothest play and cannot be changed."), llp(-1, -2, 6));
        if (optionsGroup.equals("CONTROLS"))
            list.addView(actionRow("EDIT LAYOUT", "Move each part of the touch pad and set its own size and opacity.",
                () -> startActivity(new Intent(this, ControlsEditorActivity.class))), llp(-1, -2, 6));
        if (optionsGroup.equals("DEBUG")) {
            list.addView(actionRow("CUSTOM VARIABLES", "Any other runtime switch, one NAME=VALUE per line (advanced).", this::editCustom), llp(-1, -2, 6));
        }
    }

    private TextView note(String s) {
        TextView t = text(s, 11, STEEL, regular);
        t.setLineSpacing(0, 1.15f);
        t.setPadding(dp(6), dp(2), dp(6), dp(6));
        return t;
    }

    private View optionRow(Settings.Opt o, LinkedHashMap<String, LinkedHashMap<String, String>> ini) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(14), dp(8), dp(10), dp(8));
        row.setBackground(panel(175));
        row.setFocusable(true);
        LinearLayout labels = new LinearLayout(this);
        labels.setOrientation(LinearLayout.VERTICAL);
        TextView name = text(o.label.toUpperCase(java.util.Locale.ROOT), 14.5f, SILVER, semi);
        name.setLetterSpacing(0.06f);
        labels.addView(name);
        TextView help = text(o.help, 10.5f, STEEL, regular);
        help.setPadding(0, dp(3), dp(10), 0);
        labels.addView(help);
        row.addView(labels, new LinearLayout.LayoutParams(0, -2, 1f));

        final int n = Settings.choices(this, o);
        final int[] idx = { Math.min(o.index(Settings.get(this, o, ini)), n - 1) };
        TextView left = text("◀", 14, RED, bold);
        left.setPadding(dp(10), dp(6), dp(10), dp(6));
        TextView value = text(o.labels[idx[0]], 14, Color.WHITE, bold);
        value.setGravity(Gravity.CENTER);
        value.setMinWidth(dp(132));
        TextView right = text("▶", 14, RED, bold);
        right.setPadding(dp(10), dp(6), dp(10), dp(6));
        row.addView(left); row.addView(value); row.addView(right);

        Runnable apply = () -> {
            Settings.set(this, o, o.values[idx[0]], ini);
            value.setText(o.labels[idx[0]]);
            if (!noAnim()) {
                value.setScaleX(1.18f); value.setScaleY(1.18f); value.setAlpha(0.4f);
                value.animate().scaleX(1f).scaleY(1f).alpha(1f).setDuration(220).setInterpolator(new DecelerateInterpolator()).start();
            }
            if (o.key.equals("no_anim")) { if (noAnim()) stopLoops(); else startLoops(); }
        };
        left.setOnClickListener(v -> { idx[0] = (idx[0] + n - 1) % n; apply.run(); });
        right.setOnClickListener(v -> { idx[0] = (idx[0] + 1) % n; apply.run(); });
        row.setOnClickListener(v -> { idx[0] = (idx[0] + 1) % n; apply.run(); });
        row.setOnKeyListener((v, code, ev) -> {
            if (ev.getAction() != KeyEvent.ACTION_DOWN) return false;
            if (code == KeyEvent.KEYCODE_DPAD_LEFT) { idx[0] = (idx[0] + n - 1) % n; apply.run(); return true; }
            if (code == KeyEvent.KEYCODE_DPAD_RIGHT) { idx[0] = (idx[0] + 1) % n; apply.run(); return true; }
            return false;
        });
        row.setOnFocusChangeListener((v, f) -> row.setBackground(f ? focusPanel() : panel(175)));
        tapFeedback(left); tapFeedback(right);
        return row;
    }

    private GradientDrawable focusPanel() {
        GradientDrawable g = panel(200);
        g.setStroke(dp(2), RED);
        return g;
    }

    private View actionRow(String title, String help, Runnable action) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(14), dp(10), dp(14), dp(10));
        row.setBackground(panel(175));
        row.setFocusable(true);
        LinearLayout labels = new LinearLayout(this);
        labels.setOrientation(LinearLayout.VERTICAL);
        TextView name = text(title, 14.5f, SILVER, semi);
        name.setLetterSpacing(0.06f);
        labels.addView(name);
        TextView h = text(help, 10.5f, STEEL, regular);
        h.setPadding(0, dp(3), 0, 0);
        labels.addView(h);
        row.addView(labels, new LinearLayout.LayoutParams(0, -2, 1f));
        row.addView(text("▶", 14, RED, bold));
        row.setOnClickListener(v -> action.run());
        row.setOnFocusChangeListener((v, f) -> row.setBackground(f ? focusPanel() : panel(175)));
        tapFeedback(row);
        return row;
    }

    private void fillTools(LinearLayout list) {
        list.addView(note("Game folder, saves and logs: " + dataRoot(this) + "\nSaves: TTT2Recomp/saves   Log: TTT2Recomp/log.txt"));
        list.addView(actionRow(installed() ? "REINSTALL GAME" : "INSTALL GAME", "Builds the game folder again from the chosen ISO or game folder and Title Update.", this::startInstall), llp(-1, -2, 6));
        list.addView(actionRow(shadersBuilt(this) ? "BUILD SHADERS (DONE - CHECK FOR NEW)" : "BUILD SHADERS",
            "Prepares every graphics shader now (one time, 10-40 minutes) so matches start without missing stages or characters.", this::startShaderBuild), llp(-1, -2, 6));
        list.addView(actionRow("ADD DLC", "Your own DLC packages (Bonus Tracks, Bonus Movies...). Installed now: " + dlcCount() + ".", this::chooseDlc), llp(-1, -2, 6));
        if (dlcCount() > 0) list.addView(actionRow("REMOVE DLC", "Takes the added DLC out of the game.", this::removeDlc), llp(-1, -2, 6));
        list.addView(actionRow("EDIT TOUCH CONTROLS", "Move, resize and fade each on-screen button.", () -> startActivity(new Intent(this, ControlsEditorActivity.class))), llp(-1, -2, 6));
        list.addView(actionRow("SAVE LOGS", "Saves the logs as a zip in Download/TTT2 Logs on your phone, to send with a problem report.", this::exportDebug), llp(-1, -2, 6));
        list.addView(actionRow("CHECK FOR UPDATES", "Asks volvisionmods.com for a newer version.",
            () -> VolvisionUpdateChannel.checkNow(this, VolvisionUpdateChannel.versionName(this))), llp(-1, -2, 6));
        list.addView(actionRow("LOGS", "The crash report, the start of the log, important lines and the end.", this::showLog), llp(-1, -2, 6));
        if (Settings.devUnlocked(this))
            list.addView(actionRow("FRAMES", "The frames saved by DEBUG -> SAVE FRAMES.", this::showFrames), llp(-1, -2, 6));
        list.addView(actionRow("CLEAR SHADER CACHE", "Deletes the compiled shaders and the Vulkan pipeline cache (they are built again while playing).", this::clearShaderCache), llp(-1, -2, 6));
        list.addView(actionRow("RESET ALL OPTIONS", "Every option back to its default (custom variables are kept).", () ->
            new AlertDialog.Builder(this).setTitle("Reset all options?").setPositiveButton("Reset", (d, w) -> {
                Settings.resetAll(this);
                toastStatus("Options reset to the defaults.");
            }).setNegativeButton("Cancel", null).show()), llp(-1, -2, 6));
    }

    private void editCustom() {
        EditText e = new EditText(this);
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        e.setTypeface(Typeface.MONOSPACE);
        e.setMinLines(5);
        e.setText(prefs.getString("env_custom", ""));
        e.setHint("TTT2_EXAMPLE=1");
        new AlertDialog.Builder(this).setTitle("Custom variables").setView(e)
            .setPositiveButton("Save", (d, w) -> prefs.edit().putString("env_custom", e.getText().toString()).apply())
            .setNegativeButton("Cancel", null).show();
    }

    private void clearShaderCache() {
        File root = new File(dataRoot(this));
        int n = 0;
        for (File d : new File[] { new File(root, "TTT2Recomp"), new File(root, "TTT2Recomp/shadercache"), new File(root, "TTT2Recomp/shadercache/ucode"), new File(root, "ttt2/port/shadercache-debug"), new File(root, "TTT2Recomp/native_spv") }) {
            File[] files = d.listFiles();
            if (files == null) continue;
            for (File f : files) {
                String s = f.getName();
                if (f.isFile() && (s.endsWith(".dxbc") || s.endsWith(".dxvk-cache") || s.endsWith(".bin") || s.endsWith(".spv") || s.equals("prebuilt.txt")) && f.delete()) n++;
            }
        }
        toastStatus("Shader cache cleared (" + n + " files).");
    }

    private void toastStatus(String s) {
        android.widget.Toast.makeText(this, s, android.widget.Toast.LENGTH_SHORT).show();
        if (status != null) status.setText(s);
    }

    // ================================================================ CREDITS
    /** r23: Catonix's profile card (photo, name, what the port is, the links, three buttons), then the work it is built on. */
    private View buildCredits() {
        ScrollView sv = new ScrollView(this);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(4), dp(4), dp(4), dp(24));
        sv.addView(col);

        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(22), dp(20), dp(22), dp(20));
        card.setBackground(card(225));
        LinearLayout top = new LinearLayout(this);
        top.setOrientation(LinearLayout.HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.addView(catonixPhoto(), new LinearLayout.LayoutParams(dp(96), dp(96)));
        LinearLayout info = new LinearLayout(this);
        info.setOrientation(LinearLayout.VERTICAL);
        info.setPadding(dp(22), 0, 0, 0);
        TextView name = text(Credits.PORT_BY, 34, Color.WHITE, bold);
        name.setLetterSpacing(0.04f);
        info.addView(name);
        info.addView(text(Credits.SUBTITLE, 14, SILVER, regular), llp(-2, -2, 6));
        for (String[] l : Credits.LINKS) {
            LinearLayout line = new LinearLayout(this);
            line.setGravity(Gravity.CENTER_VERTICAL);
            line.addView(text(l[0] + "  ", 12.5f, STEEL, regular));
            TextView url = text(l[1], 12.5f, SILVER, regular);
            url.setFocusable(true);
            url.setOnClickListener(v -> open(l[1]));
            line.addView(url);
            if (!l[2].isEmpty()) line.addView(text("  " + l[2], 12.5f, STEEL, regular));
            info.addView(line, llp(-2, -2, 5));
        }
        top.addView(info, new LinearLayout.LayoutParams(0, -2, 1f));
        card.addView(top);
        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        for (int i = 0; i < Credits.BUTTONS.length; i++) {
            final String[] b = Credits.BUTTONS[i];
            TextView btn = text(b[0], 14, Color.WHITE, semi);
            btn.setGravity(Gravity.CENTER);
            btn.setPadding(dp(8), dp(11), dp(8), dp(11));
            GradientDrawable g = new GradientDrawable();
            g.setColor(Color.rgb(214, 24, 44));
            g.setCornerRadius(dp(10));
            btn.setBackground(g);
            btn.setFocusable(true);
            btn.setOnClickListener(v -> open(b[1]));
            tapFeedback(btn);
            LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(0, -2, 1f);
            if (i > 0) bp.leftMargin = dp(10);
            buttons.addView(btn, bp);
        }
        card.addView(buttons, llp(-1, -2, 18));
        col.addView(card);

        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(dp(22), dp(16), dp(22), dp(16));
        list.setBackground(card(200));
        TextView built = text("BUILT WITH", 11, RED, semi);
        built.setLetterSpacing(0.25f);
        list.addView(built);
        for (String[] t : Credits.THANKS) {
            TextView h = text(t[0], 13.5f, SILVER, semi);
            list.addView(h, llp(-1, -2, 10));
            TextView d = text(t[1], 11.5f, STEEL, regular);
            d.setPadding(0, dp(2), 0, 0);
            list.addView(d);
        }
        col.addView(list, llp(-1, -2, 12));

        // r28: the version; tapping it 7 times turns the developer settings on or off (for testing only).
        TextView ver = text("VERSION " + VolvisionUpdateChannel.versionName(this) + (Settings.devUnlocked(this) ? "   (developer settings on)" : ""), 11, STEEL, semi);
        ver.setLetterSpacing(0.15f);
        ver.setPadding(dp(6), 0, dp(6), 0);
        final int[] taps = { 0 };
        ver.setOnClickListener(v -> {
            if (++taps[0] < 7) return;
            taps[0] = 0;
            boolean on = !Settings.devUnlocked(this);
            Settings.setDevUnlocked(this, on);
            if (!on) Settings.enforce(this);
            ver.setText("VERSION " + VolvisionUpdateChannel.versionName(this) + (on ? "   (developer settings on)" : ""));
            toastStatus(on ? "Developer settings on (Options shows everything)." : "Developer settings off (tested values restored).");
        });
        col.addView(ver, llp(-1, -2, 12));

        TextView disc = text(Credits.DISCLAIMER, 10.5f, STEEL, regular);
        disc.setLineSpacing(0, 1.15f);
        disc.setPadding(dp(6), 0, dp(6), 0);
        col.addView(disc, llp(-1, -2, 12));

        if (!noAnim()) {
            for (int i = 0; i < col.getChildCount(); i++) {
                View v = col.getChildAt(i);
                v.setAlpha(0f);
                v.setTranslationY(dp(18));
                v.animate().alpha(1f).translationY(0).setStartDelay(160 + i * 80L).setDuration(300).setInterpolator(new DecelerateInterpolator()).start();
            }
        }
        return page("CREDITS", sv);
    }

    /** The credits' cards: near-black, rounded, a thin light border. */
    private GradientDrawable card(int alpha) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(Color.argb(alpha, 8, 6, 10));
        g.setStroke(dp(1), Color.argb(70, 220, 224, 235));
        g.setCornerRadius(dp(18));
        return g;
    }

    /** Catonix's profile photo (res/drawable-nodpi/catonix.png), round with a light ring. */
    private View catonixPhoto() {
        int id = getResources().getIdentifier("catonix", "drawable", getPackageName());
        View v;
        if (id != 0) {
            ImageView iv = new ImageView(this);
            iv.setImageResource(id);
            iv.setScaleType(ImageView.ScaleType.CENTER_CROP);
            v = iv;
        } else {
            TextView t = text("C", 40, Color.WHITE, bold);
            t.setGravity(Gravity.CENTER);
            GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[] { RED, RED_DARK });
            g.setShape(GradientDrawable.OVAL);
            t.setBackground(g);
            v = t;
        }
        v.setOutlineProvider(new ViewOutlineProvider() {
            @Override public void getOutline(View view, Outline o) { o.setOval(0, 0, view.getWidth(), view.getHeight()); }
        });
        v.setClipToOutline(true);
        v.setForeground(ring());
        return v;
    }

    private GradientDrawable ring() {
        GradientDrawable g = new GradientDrawable();
        g.setShape(GradientDrawable.OVAL);
        g.setStroke(dp(3), Color.rgb(236, 236, 240));
        return g;
    }

    private TextView link(String label, String url) {
        TextView t = text(label, 12, BLUE, regular);
        t.setPadding(0, dp(3), 0, 0);
        t.setFocusable(true);
        t.setOnClickListener(v -> open(url));
        return t;
    }

    private void open(String url) {
        try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url))); }
        catch (Exception e) { toastStatus("No app can open " + url); }
    }

    // ================================================================ state
    private void refresh() {
        if (playItem == null) return;
        boolean ok = installed();
        String iso = prefs.getString("iso", null), tu = prefs.getString("tu", null), folder = prefs.getString("folder", null);
        isoItem.setSub(folder != null ? "FOLDER: " + folderName(folder) : iso == null ? "ISO OR GAME FOLDER - NOT CHOSEN" : nameOf(iso, ""));
        tuItem.setSub(tu == null ? (folder != null ? "NOT NEEDED FOR AN INSTALLED FOLDER" : "NOT CHOSEN") : nameOf(tu, ""));
        final boolean ready = folder != null || (iso != null && tu != null);
        playItem.setSub(installing ? "PLEASE WAIT..." : ok ? (shadersBuilt(this) ? "READY" : "READY - SHADERS NOT BUILT YET (OPTIONS > TOOLS)") : ready ? "TAP TO INSTALL" : "SELECT GAME AND TITLE UPDATE FIRST");
        playItem.setAlpha(installing ? 0.5f : 1f);
        isoItem.setEnabled(!installing);
        tuItem.setEnabled(!installing);
        if (!installing && status.getText().length() == 0)
            status.setText(ok ? "Game installed." : "Choose your disc image (or extracted game folder) and Title Update 3 (about 10 GB free space needed).");
    }

    private String nameOf(String uri, String fallback) {
        if (uri == null) return fallback;
        try (Cursor c = getContentResolver().query(Uri.parse(uri), new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) {}
        return Uri.parse(uri).getLastPathSegment();
    }

    private void pick(int code) {
        if (installing) return;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, code);
    }

    // r30: the game from a disc image or from a folder (extracted from the disc, or an installed game folder).
    private void chooseGameSource() {
        if (installing) return;
        new AlertDialog.Builder(this).setTitle("Select your game")
            .setItems(new String[] { "Disc image (.iso)", "Extracted game folder" }, (d, which) -> {
                if (which == 0) pick(PICK_ISO);
                else {
                    new AlertDialog.Builder(this).setTitle("Extracted game folder")
                        .setMessage("Choose the folder that holds default.xex, nrfo.bin and the data000-041.bin files (extracted from your disc), "
                            + "or an installed game folder with tekken.img and the update folder.\n\nIf Title Update 3 is not inside the folder, "
                            + "select it with SELECT TITLE UPDATE.")
                        .setPositiveButton("Choose folder", (d2, w2) -> pickTree(PICK_FOLDER)).setNegativeButton("Cancel", null).show();
                }
            }).show();
    }

    private void pickTree(int code) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, code);
    }

    private String folderName(String treeUri) {
        try {
            String id = android.provider.DocumentsContract.getTreeDocumentId(Uri.parse(treeUri));
            int c = id.indexOf(':');
            String path = c >= 0 ? id.substring(c + 1) : id;
            return path.isEmpty() ? "/" : path;
        } catch (Exception e) { return "chosen"; }
    }

    /** A tree's direct children: {name, document uri, mime}. */
    private List<String[]> children(Uri tree, String parentDocId) {
        List<String[]> out = new ArrayList<>();
        Uri kids = android.provider.DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentDocId);
        try (Cursor c = getContentResolver().query(kids, new String[] {
                android.provider.DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                android.provider.DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                android.provider.DocumentsContract.Document.COLUMN_MIME_TYPE }, null, null, null)) {
            while (c != null && c.moveToNext())
                out.add(new String[] { c.getString(1), android.provider.DocumentsContract.buildDocumentUriUsingTree(tree, c.getString(0)).toString(),
                                       c.getString(2), c.getString(0) });
        } catch (Exception ignored) {}
        return out;
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null) return;
        if (request == PICK_DLC_FILES || request == PICK_DLC_DIR) { addDlc(request, data); return; }
        if (data.getData() == null) return;
        Uri uri = data.getData();
        try { getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); } catch (Exception ignored) {}
        if (request == PICK_FOLDER) prefs.edit().putString("folder", uri.toString()).remove("iso").apply();
        else if (request == PICK_ISO) prefs.edit().putString("iso", uri.toString()).remove("folder").apply();
        else prefs.edit().putString("tu", uri.toString()).apply();
        status.setText("");
        refresh();
        // Ready and nothing installed yet: offer the install straight away.
        if (!installed() && (prefs.getString("folder", null) != null || (prefs.getString("iso", null) != null && prefs.getString("tu", null) != null)))
            new AlertDialog.Builder(this).setTitle("Install the game?")
                .setMessage("Builds the game folder from your game files and Title Update 3 (about 10 GB free space, several minutes).")
                .setPositiveButton("Install", (d, w) -> startInstall()).setNegativeButton("Later", null).show();
    }

    // ================================================================ install
    private View buildInstallPanel() {
        FrameLayout f = new FrameLayout(this);
        f.setClickable(true);
        View shade = new View(this);
        shade.setBackgroundColor(Color.argb(200, 4, 3, 8));
        f.addView(shade, flp(-1, -1, Gravity.FILL));
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(26), dp(20), dp(26), dp(18));
        card.setBackground(panel(235));
        installTitle = text("INSTALLING", 22, Color.WHITE, bold);
        installTitle.setLetterSpacing(0.12f);
        card.addView(installTitle);
        installText = text("", 12, STEEL, regular);
        card.addView(installText, llp(-1, -2, 8));
        installBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        installBar.setMax(100);
        installBar.setProgressTintList(android.content.res.ColorStateList.valueOf(RED));
        card.addView(installBar, llp(-1, dp(8), 10));
        TextView cancelB = text("CANCEL", 13, Color.WHITE, semi);
        cancelB.setLetterSpacing(0.12f);
        cancelB.setPadding(dp(20), dp(7), dp(18), dp(7));
        cancelB.setBackground(new Plate(dp(9), Color.argb(200, 60, 10, 16), RED));
        cancelB.setFocusable(true);
        cancelB.setOnClickListener(v -> { cancel = true; installText.setText("Cancelling..."); });
        tapFeedback(cancelB);
        LinearLayout.LayoutParams cp = llp(-2, -2, 14);
        cp.gravity = Gravity.END;
        card.addView(cancelB, cp);
        f.addView(card, flp(dp(460), -2, Gravity.CENTER));
        return f;
    }

    private void showInstallPanel(boolean show) {
        if (noAnim()) { installPanel.setVisibility(show ? View.VISIBLE : View.GONE); installPanel.setAlpha(1f); return; }
        if (show) {
            installPanel.setVisibility(View.VISIBLE);
            installPanel.setAlpha(0f);
            installPanel.setScaleX(1.04f); installPanel.setScaleY(1.04f);
            installPanel.animate().alpha(1f).scaleX(1f).scaleY(1f).setDuration(260).setListener(null).start();
        } else {
            installPanel.animate().alpha(0f).setDuration(220).setListener(new AnimatorListenerAdapter() {
                @Override public void onAnimationEnd(Animator a) { installPanel.animate().setListener(null); installPanel.setVisibility(View.GONE); }
            }).start();
        }
    }

    private void startInstall() {
        if (installing) return;
        final String folder = prefs.getString("folder", null);
        if (folder == null && (prefs.getString("iso", null) == null || prefs.getString("tu", null) == null)) {
            toastStatus("Select your game (ISO or folder) and Title Update first.");
            return;
        }
        installing = true;
        cancel = false;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (current != home) goHome();
        installTitle.setText("INSTALLING");
        installText.setText("Starting...");
        installBar.setProgress(0);
        showInstallPanel(true);
        refresh();
        if (folder != null) { new Thread(() -> installFromFolder(Uri.parse(folder)), "ttt2-install").start(); return; }
        new Thread(() -> {
            String error;
            try (ParcelFileDescriptor iso = getContentResolver().openFileDescriptor(Uri.parse(prefs.getString("iso", "")), "r");
                 ParcelFileDescriptor tu = getContentResolver().openFileDescriptor(Uri.parse(prefs.getString("tu", "")), "r")) {
                if (iso == null || tu == null) throw new Exception("Could not open the chosen files.");
                error = Native.install("/proc/self/fd/" + iso.getFd(), "/proc/self/fd/" + tu.getFd(), dataRoot(this), new Native.Listener() {
                    @Override public void progress(int percent, String text) {
                        runOnUiThread(() -> { installBar.setProgress(percent); installText.setText(text); });
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
                showInstallPanel(false);
                status.setText(err == null ? "Installed. Press PLAY." : "Install failed: " + err);
                refresh();
                if (err == null) startShaderBuild();
            });
        }, "ttt2-install").start();
    }

    /** r30: the folder's files (and its update/ folder) opened and handed to Native.installFolder. Worker thread. */
    private void installFromFolder(Uri tree) {
        String error;
        List<ParcelFileDescriptor> open = new ArrayList<>();
        try {
            String root = android.provider.DocumentsContract.getTreeDocumentId(tree);
            List<String> names = new ArrayList<>(), paths = new ArrayList<>();
            for (String[] k : children(tree, root)) {
                if (android.provider.DocumentsContract.Document.MIME_TYPE_DIR.equals(k[2])) {
                    if (!k[0].equalsIgnoreCase("update")) continue;
                    for (String[] u : children(tree, k[3])) {
                        if (android.provider.DocumentsContract.Document.MIME_TYPE_DIR.equals(u[2])) continue;
                        ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(Uri.parse(u[1]), "r");
                        if (pfd == null) continue;
                        open.add(pfd); names.add("update/" + u[0]); paths.add("/proc/self/fd/" + pfd.getFd());
                    }
                    continue;
                }
                ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(Uri.parse(k[1]), "r");
                if (pfd == null) continue;
                open.add(pfd); names.add(k[0]); paths.add("/proc/self/fd/" + pfd.getFd());
            }
            if (names.isEmpty()) throw new Exception("The folder is empty or could not be read.");
            ParcelFileDescriptor tu = null;
            String tuUri = prefs.getString("tu", null);
            if (tuUri != null) { tu = getContentResolver().openFileDescriptor(Uri.parse(tuUri), "r"); if (tu != null) open.add(tu); }
            error = Native.installFolder(names.toArray(new String[0]), paths.toArray(new String[0]), tu != null ? "/proc/self/fd/" + tu.getFd() : "",
                dataRoot(this), new Native.Listener() {
                    @Override public void progress(int percent, String text) {
                        runOnUiThread(() -> { installBar.setProgress(percent); installText.setText(text); });
                    }
                    @Override public boolean cancelled() { return cancel; }
                });
        } catch (Throwable t) {
            error = t.getMessage() != null ? t.getMessage() : t.toString();
        } finally {
            for (ParcelFileDescriptor p : open) try { p.close(); } catch (Exception ignored) {}
        }
        final String err = error;
        runOnUiThread(() -> {
            installing = false;
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            showInstallPanel(false);
            status.setText(err == null ? "Installed. Press PLAY." : "Install failed: " + err);
            refresh();
            if (err == null) startShaderBuild();
        });
    }

    // ================================================================ DLC (r30)
    private int dlcCount() {
        File[] f = new File(dataRoot(this), "game/dlc").listFiles((d, n) -> n.endsWith(".ttt2-dlc"));
        return f == null ? 0 : f.length;
    }

    private void chooseDlc() {
        if (installing) return;
        if (!installed()) { toastStatus("Install the game first."); return; }
        new AlertDialog.Builder(this).setTitle("Add DLC")
            .setMessage("Adds your own Tekken Tag Tournament 2 DLC (Xbox 360 content packages, e.g. the Bonus Tracks and Bonus Movies packs). "
                + "Choose the package files, or a folder that holds them.")
            .setPositiveButton("Choose files", (d, w) -> {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
                startActivityForResult(i, PICK_DLC_FILES);
            })
            .setNeutralButton("Choose folder", (d, w) -> pickTree(PICK_DLC_DIR))
            .setNegativeButton("Cancel", null).show();
    }

    /** The chosen packages copied to a temporary folder (names kept), checked and extracted by Native.installDlc. */
    private void addDlc(int request, Intent data) {
        final List<String[]> picked = new ArrayList<>();       // {name, uri}
        if (request == PICK_DLC_FILES) {
            ClipData cd = data.getClipData();
            if (cd != null) for (int i = 0; i < cd.getItemCount(); i++) picked.add(new String[] { nameOf(cd.getItemAt(i).getUri().toString(), "dlc" + i), cd.getItemAt(i).getUri().toString() });
            else if (data.getData() != null) picked.add(new String[] { nameOf(data.getData().toString(), "dlc"), data.getData().toString() });
        } else if (data.getData() != null) {
            Uri tree = data.getData();
            collectDlc(tree, android.provider.DocumentsContract.getTreeDocumentId(tree), "", 0, picked);
        }
        if (picked.isEmpty()) { toastStatus("No files chosen."); return; }
        installing = true;
        cancel = false;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        installTitle.setText("ADDING DLC");
        installText.setText("Copying...");
        installBar.setProgress(0);
        showInstallPanel(true);
        new Thread(() -> {
            String msg;
            File tmp = new File(dataRoot(this), "tmp/dlc_in");
            deleteTree(tmp);
            try {
                if (!tmp.mkdirs() && !tmp.isDirectory()) throw new Exception("Could not create a temporary folder.");
                byte[] buf = new byte[1 << 20];
                for (int i = 0; i < picked.size() && !cancel; i++) {
                    final int n = i;
                    runOnUiThread(() -> { installText.setText("Copying " + picked.get(n)[0] + "..."); installBar.setProgress(n * 30 / picked.size()); });
                    File out = new File(tmp, picked.get(i)[0].replaceAll("[\\\\/:*?\"<>|]", "_"));
                    if (out.getParentFile() != null) out.getParentFile().mkdirs();
                    try (InputStream in = getContentResolver().openInputStream(Uri.parse(picked.get(i)[1]));
                         OutputStream os = new java.io.FileOutputStream(out)) {
                        if (in == null) continue;
                        int r;
                        while ((r = in.read(buf)) > 0 && !cancel) os.write(buf, 0, r);
                    }
                }
                if (cancel) throw new Exception("Cancelled.");
                String r = Native.installDlc(tmp.getAbsolutePath(), dataRoot(this), new Native.Listener() {
                    @Override public void progress(int percent, String text) {
                        runOnUiThread(() -> { installBar.setProgress(30 + percent * 70 / 100); installText.setText(text); });
                    }
                    @Override public boolean cancelled() { return cancel; }
                });
                msg = r != null && r.startsWith("OK:") ? r.substring(3) + " DLC package(s) added. They are in the game now." : "DLC not added: " + r;
            } catch (Throwable t) {
                msg = "DLC not added: " + (t.getMessage() != null ? t.getMessage() : t.toString());
            } finally {
                deleteTree(tmp);
            }
            final String m = msg;
            runOnUiThread(() -> {
                installing = false;
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                showInstallPanel(false);
                new AlertDialog.Builder(this).setTitle("DLC").setMessage(m).setPositiveButton("OK", null).show();
                refresh();
            });
        }, "ttt2-dlc").start();
    }

    /** Files under a chosen DLC folder (3 levels), each under 2 GB, with their folder path kept in the name. */
    private void collectDlc(Uri tree, String docId, String prefix, int depth, List<String[]> out) {
        if (depth > 3 || out.size() > 200) return;
        for (String[] k : children(tree, docId)) {
            if (android.provider.DocumentsContract.Document.MIME_TYPE_DIR.equals(k[2])) collectDlc(tree, k[3], prefix + k[0] + "/", depth + 1, out);
            else out.add(new String[] { prefix + k[0], k[1] });
        }
    }

    private static void deleteTree(File f) {
        if (f == null || !f.exists()) return;
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    private void removeDlc() {
        if (installing) return;
        int n = dlcCount();
        if (n == 0) { toastStatus("No DLC installed."); return; }
        new AlertDialog.Builder(this).setTitle("Remove DLC").setMessage("Removes the " + n + " DLC package(s) from the game (your own package files are not touched).")
            .setPositiveButton("Remove", (d, w) -> { deleteTree(new File(dataRoot(this), "game/dlc")); toastStatus("DLC removed."); })
            .setNegativeButton("Cancel", null).show();
    }

    // ================================================================ shaders (r23)
    static boolean shadersBuilt(Context c) {
        return new File(dataRoot(c), "TTT2Recomp/shadercache/prebuilt.txt").isFile();
    }

    /** Every game shader compiled ahead of time (Native.buildShaders): the match's first start no longer builds them while it runs. */
    private void startShaderBuild() {
        if (installing) return;
        if (!installed()) { toastStatus("Install the game first."); return; }
        installing = true;
        cancel = false;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (current != home) goHome();
        installTitle.setText("BUILDING SHADERS");
        installText.setText("Finding the game's shaders... (one time, 10-40 minutes; keep the app open)");
        installBar.setProgress(0);
        showInstallPanel(true);
        refresh();
        new Thread(() -> {
            String error;
            try {
                error = Native.buildShaders(dataRoot(this), new Native.Listener() {
                    @Override public void progress(int percent, String text) {
                        runOnUiThread(() -> { installBar.setProgress(percent); installText.setText(text + "\nOne time only. Keep the app open - it continues where it stopped."); });
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
                showInstallPanel(false);
                status.setText(err == null ? "Shaders built. Press PLAY." : err);
                refresh();
            });
        }, "ttt2-shaders").start();
    }

    private void play() {
        if (installing) return;
        Settings.enforce(this);
        if (installed() && !shadersBuilt(this) && !prefs.getBoolean("skip_shader_prompt", false)) {
            new AlertDialog.Builder(this).setTitle("Build shaders first?")
                .setMessage("Recommended once: every graphics shader is prepared now (10-40 minutes), so matches start with the stage and "
                    + "characters ready instead of building them while you play.")
                .setPositiveButton("Build now", (d, w) -> startShaderBuild())
                .setNegativeButton("Play anyway", (d, w) -> { prefs.edit().putBoolean("skip_shader_prompt", true).apply(); play(); })
                .show();
            return;
        }
        if (!installed()) {
            if (prefs.getString("iso", null) != null && prefs.getString("tu", null) != null) startInstall();
            else toastStatus("Select your ISO and Title Update first.");
            return;
        }
        if (noAnim()) { startActivity(new Intent(this, GameActivity.class)); return; }
        // A short flash out before the game takes over.
        root.animate().alpha(0f).setDuration(240).setListener(new AnimatorListenerAdapter() {
            @Override public void onAnimationEnd(Animator a) {
                root.animate().setListener(null);
                startActivity(new Intent(LauncherActivity.this, GameActivity.class));
                overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out);
                root.postDelayed(() -> root.setAlpha(1f), 600);
            }
        }).start();
    }

    // ================================================================ logs, frames, export (unchanged)
    private TextView label(String s, float size, int color) { return text(s, size, color, Typeface.DEFAULT); }

    private LinearLayout.LayoutParams lp(int top) { return llp(-1, -2, top); }

    private static String tail(File f, int max) {
        if (!f.isFile()) return null;
        try (InputStream in = new FileInputStream(f)) {
            byte[] b = new byte[(int) Math.min(f.length(), max)];
            long skip = f.length() - b.length;
            if (skip > 0) in.skip(skip);
            int n = in.read(b);
            return new String(b, 0, Math.max(n, 0), "UTF-8");
        } catch (Exception e) {
            return null;
        }
    }

    private static final Pattern KEY = Pattern.compile(
        "err:|warn:|error|Error|FAILED|failed|\\[android\\]|\\[gpu-hw\\] (present|device|adapter|shader)|presenting through|cannot|\\[path\\]|D3DCompile|vkd3d|Vulkan|DXVK");

    /** The first 60 lines of log.txt and up to 150 important lines (errors, renderer setup) from its first 4 MB. */
    private static String keyLines(File f) {
        if (!f.isFile()) return null;
        StringBuilder head = new StringBuilder("===== log.txt (start) =====\n");
        StringBuilder key = new StringBuilder("===== log.txt (important lines) =====\n");
        int n = 0, k = 0;
        long bytes = 0;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), "UTF-8"))) {
            String line;
            while ((line = r.readLine()) != null && bytes < 4_000_000) {
                bytes += line.length() + 1;
                if (n++ < 60) head.append(line).append('\n');
                else if (k < 150 && KEY.matcher(line).find()) { key.append(line).append('\n'); k++; }
            }
        } catch (Exception e) {
            return null;
        }
        return head.append('\n').append(key).append('\n').toString();
    }

    /** BMP (32-bit, as gpu_hw_d3d11.h WriteBmp writes it: top-down when the height is negative) to a Bitmap. */
    private static Bitmap readBmp(File f) {
        try (InputStream in = new FileInputStream(f)) {
            byte[] h = new byte[54];
            if (in.read(h) != 54 || h[0] != 'B' || h[1] != 'M') return null;
            int w = (h[18] & 255) | (h[19] & 255) << 8 | (h[20] & 255) << 16 | h[21] << 24;
            int hh = (h[22] & 255) | (h[23] & 255) << 8 | (h[24] & 255) << 16 | h[25] << 24;
            boolean topDown = hh < 0;
            int ht = Math.abs(hh);
            if (w <= 0 || ht <= 0 || w > 8192 || ht > 8192) return null;
            byte[] px = new byte[w * ht * 4];
            int got = 0;
            while (got < px.length) { int r = in.read(px, got, px.length - got); if (r < 0) break; got += r; }
            int[] argb = new int[w * ht];
            for (int y = 0; y < ht; y++) {
                int row = topDown ? y : ht - 1 - y;
                for (int x = 0; x < w; x++) {
                    int i = (row * w + x) * 4;
                    argb[y * w + x] = 0xFF000000 | (px[i + 2] & 255) << 16 | (px[i + 1] & 255) << 8 | (px[i] & 255);
                }
            }
            return Bitmap.createBitmap(argb, w, ht, Bitmap.Config.ARGB_8888);
        } catch (Exception | OutOfMemoryError e) {
            return null;
        }
    }

    private static void zipDir(ZipOutputStream z, File dir, String prefix, long maxEach) throws java.io.IOException {
        File[] files = dir.listFiles();
        if (files == null) return;
        byte[] buf = new byte[65536];
        for (File f : files) {
            if (f.isDirectory()) { zipDir(z, f, prefix + f.getName() + "/", maxEach); continue; }
            String n = f.getName();
            if (n.endsWith(".dxbc") || n.endsWith(".bin") || n.endsWith(".spv") || n.endsWith(".tmp") || n.endsWith(".dxvk-cache") || f.length() > maxEach) continue;
            if (prefix.endsWith("native_spv/")) continue;
            z.putNextEntry(new ZipEntry(prefix + n));
            try (InputStream in = new FileInputStream(f)) { int r; while ((r = in.read(buf)) > 0) z.write(buf, 0, r); }
            z.closeEntry();
        }
    }

    /** r28: the logs, the self-test and settings as Download/TTT2 Logs/TTT2-Logs-<time>.zip (internal storage), in the background. */
    private void exportDebug() {
        final String name = "TTT2-Logs-" + new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US).format(new java.util.Date()) + ".zip";
        toastStatus("Saving logs…");
        new Thread(() -> {
            String msg;
            try {
                ContentValues v = new ContentValues();
                v.put(MediaStore.Downloads.DISPLAY_NAME, name);
                v.put(MediaStore.Downloads.MIME_TYPE, "application/zip");
                v.put(MediaStore.Downloads.RELATIVE_PATH, android.os.Environment.DIRECTORY_DOWNLOADS + "/TTT2 Logs");
                v.put(MediaStore.Downloads.IS_PENDING, 1);
                Uri uri = getContentResolver().insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
                if (uri == null) throw new Exception("could not create the file in Download");
                try (OutputStream os = getContentResolver().openOutputStream(uri); ZipOutputStream z = new ZipOutputStream(os)) {
                    z.putNextEntry(new ZipEntry("about.txt"));
                    z.write(("Tekken Tag 2 for Android v" + VolvisionUpdateChannel.versionName(this) + "\n"
                        + "Device: " + android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL + ", Android " + android.os.Build.VERSION.RELEASE + "\n"
                        + "Saved: " + new java.util.Date() + "\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
                    z.closeEntry();
                    zipDir(z, new File(dataRoot(this), "TTT2Recomp"), "TTT2Recomp/", 40_000_000L);
                    zipDir(z, new File(dataRoot(this), "ttt2/port"), "ttt2/port/", 40_000_000L);
                }
                ContentValues done = new ContentValues();
                done.put(MediaStore.Downloads.IS_PENDING, 0);
                getContentResolver().update(uri, done, null, null);
                msg = "Logs saved: Download/TTT2 Logs/" + name;
            } catch (Exception e) {
                msg = "Saving the logs failed: " + e.getMessage();
            }
            final String m = msg;
            runOnUiThread(() -> {
                toastStatus(m);
                if (!isFinishing()) new AlertDialog.Builder(this).setTitle("Logs").setMessage(m).setPositiveButton("OK", null).show();
            });
        }, "save-logs").start();
    }

    /** Debug: the game's rendered frame (fb_native.bmp) and the window's back buffer (fb_back.bmp). */
    private void showFrames() {
        File dir = new File(dataRoot(this), "ttt2/port");
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(10), dp(10), dp(10), dp(10));
        for (String name : new String[] { "fb_native.bmp", "fb_back.bmp", "fbdump.bmp" }) {
            File f = new File(dir, name);
            col.addView(label(name + (f.isFile() ? "  (" + f.length() / 1024 + " KB, " + new java.util.Date(f.lastModified()) + ")" : "  - not saved yet"), 13, Color.WHITE));
            Bitmap b = f.isFile() ? readBmp(f) : null;
            if (b != null) {
                ImageView iv = new ImageView(this);
                iv.setImageBitmap(b);
                iv.setAdjustViewBounds(true);
                col.addView(iv, lp(4));
            }
        }
        col.addView(label("Turn on \"Debug: save frames\", PLAY for 20 seconds, come back here.", 12, Color.GRAY), lp(8));
        ScrollView sv = new ScrollView(this);
        sv.addView(col);
        new AlertDialog.Builder(this).setTitle("Frames").setView(sv).setNegativeButton("Close", null).show();
    }

    /** crash.txt (if the last start crashed), log_start.txt, the end of log.txt, the DXVK logs. */
    private void showLog() {
        File dir = new File(dataRoot(this), "TTT2Recomp");
        StringBuilder sb = new StringBuilder();
        String crash = tail(new File(dir, "crash.txt"), 20_000);
        if (crash != null && !crash.isEmpty()) sb.append("===== crash.txt =====\n").append(crash).append("\n");
        String gst = tail(new File(dir, "gpu_selftest.txt"), 8_000);
        if (gst != null) sb.append("===== gpu_selftest.txt =====\n").append(gst).append("\n");
        String start = tail(new File(dir, "log_start.txt"), 4_000);
        if (start != null) sb.append("===== log_start.txt =====\n").append(start).append("\n");
        String key = keyLines(new File(dir, "log.txt"));
        if (key != null) sb.append(key);
        String log = tail(new File(dir, "log.txt"), 60_000);
        sb.append("===== log.txt (end) =====\n").append(log != null ? log : "no log yet (" + dir + "/log.txt)").append("\n");
        File[] others = dir.listFiles((d, name) -> name.endsWith(".log"));
        if (others != null)
            for (File f : others) {
                String t = tail(f, 20_000);
                if (t != null) sb.append("===== ").append(f.getName()).append(" (end) =====\n").append(t).append("\n");
            }
        final String all = sb.toString();
        TextView tv = label(all, 11, Color.WHITE);
        tv.setTypeface(Typeface.MONOSPACE);
        tv.setTextIsSelectable(true);
        tv.setPadding(dp(10), dp(10), dp(10), dp(10));
        ScrollView sv = new ScrollView(this);
        sv.addView(tv);
        new AlertDialog.Builder(this).setTitle("Logs").setView(sv)
            .setPositiveButton("Copy all", (d, w) -> ((ClipboardManager) getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("ttt2 log", all)))
            .setNeutralButton("Share", (d, w) -> {
                Intent i = new Intent(Intent.ACTION_SEND);
                i.setType("text/plain");
                i.putExtra(Intent.EXTRA_SUBJECT, "TTT2 Android log");
                i.putExtra(Intent.EXTRA_TEXT, all.length() > 90_000 ? all.substring(all.length() - 90_000) : all);
                startActivity(Intent.createChooser(i, "Share log"));
            })
            .setNegativeButton("Close", null).show();
    }

}
