package com.saavan.ttt2;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.DashPathEffect;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.view.MotionEvent;
import android.view.View;

/**
 * On-screen pad drawn over the game (r23 redesign). Left: joystick (d-pad bits + left stick). Right: face buttons in the
 * Xbox diamond and colours (Y yellow top, A green bottom, X blue left, B red right), shoulders and triggers above,
 * Back/Start at the top middle. Fed to XInput slot 0 through Native.setTouchPad.
 *
 * Four groups - STICK, BUTTONS, SHOULDERS, MENU - each with its own position, size and opacity (prefs tc_<group>_x/y/s/a,
 * set in ControlsEditorActivity; the whole pad's opacity and size from OPTIONS > CONTROLS multiply them).
 * Press feedback: the button brightens, shrinks a little and a glow ring opens (animated per frame, ~150 ms).
 */
public class TouchOverlay extends View {
    static final int UP = 0x1, DOWN = 0x2, LEFT = 0x4, RIGHT = 0x8, START = 0x10, BACK = 0x20,
        LB = 0x100, RB = 0x200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
    static final int LT = 0x10000, RT = 0x20000; // overlay-only bits, sent as trigger values

    static final int G_STICK = 0, G_FACE = 1, G_SHOULDER = 2, G_MENU = 3;
    static final String[] GROUP_NAMES = { "STICK", "BUTTONS", "SHOULDERS", "START / BACK" };
    private static final String[] GROUP_KEYS = { "stick", "face", "shoulder", "menu" };

    private static final class Btn {
        final int bit, group, color; final String text;
        float cx, cy, r; boolean rect; final RectF box = new RectF();
        float press;            // 0..1, eased toward the pressed state
        Btn(int bit, String text, int group, int color) { this.bit = bit; this.text = text; this.group = group; this.color = color; }
        boolean hit(float x, float y) {
            if (rect) { RectF b = new RectF(box); b.inset(-box.height() * 0.2f, -box.height() * 0.2f); return b.contains(x, y); }
            float dx = x - cx, dy = y - cy; return dx * dx + dy * dy <= r * r * 1.35f;
        }
    }

    private final Btn[] buttons = {
        new Btn(A, "A", G_FACE, Color.rgb(88, 200, 60)), new Btn(B, "B", G_FACE, Color.rgb(230, 52, 52)),
        new Btn(X, "X", G_FACE, Color.rgb(48, 128, 240)), new Btn(Y, "Y", G_FACE, Color.rgb(250, 190, 40)),
        new Btn(LB, "LB", G_SHOULDER, 0), new Btn(RB, "RB", G_SHOULDER, 0), new Btn(LT, "LT", G_SHOULDER, 0), new Btn(RT, "RT", G_SHOULDER, 0),
        new Btn(START, "START", G_MENU, 0), new Btn(BACK, "BACK", G_MENU, 0),
    };
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG),
        dash = new Paint(Paint.ANTI_ALIAS_FLAG);
    private float stickCx, stickCy, stickR, stickPress;
    private int stickPointer = -1;
    private float knobX, knobY;
    private int lastState = -1, lastLx, lastLy, pressed;
    private final float density;

    // Per-group layout: offset (fraction of the view's size), scale, opacity.
    final float[] gx = new float[4], gy = new float[4], gs = new float[4], ga = new float[4];
    private float globalScale = 1f;
    private final float[] anchorX = new float[4], anchorY = new float[4];
    private final RectF[] groupBox = { new RectF(), new RectF(), new RectF(), new RectF() };

    // Editor mode (ControlsEditorActivity): drag a group to move it; the selected one is outlined.
    boolean editMode;
    int selected = G_FACE;
    interface SelectionListener { void selected(int group); }
    SelectionListener selectionListener;
    private int dragGroup = -1;
    private float dragX, dragY, dragGx, dragGy;

    public TouchOverlay(Context c) {
        super(c);
        density = getResources().getDisplayMetrics().density;
        stroke.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        dash.setStyle(Paint.Style.STROKE);
        dash.setStrokeWidth(2 * density);
        dash.setColor(Color.argb(220, 255, 70, 70));
        dash.setPathEffect(new DashPathEffect(new float[] { 10 * density, 6 * density }, 0));
        setFocusable(false);
        loadLayout();
    }

    void loadLayout() {
        SharedPreferences p = getContext().getSharedPreferences("ttt2", Context.MODE_PRIVATE);
        globalScale = Math.max(0.6f, Math.min(1.5f, p.getInt("touch_size", 100) / 100f));
        float globalAlpha = Math.max(0.15f, Math.min(1f, p.getInt("touch_alpha", 100) / 100f));
        setAlpha(editMode ? 1f : globalAlpha);
        for (int g = 0; g < 4; g++) {
            gx[g] = p.getFloat("tc_" + GROUP_KEYS[g] + "_x", 0f);
            gy[g] = p.getFloat("tc_" + GROUP_KEYS[g] + "_y", 0f);
            gs[g] = p.getFloat("tc_" + GROUP_KEYS[g] + "_s", 1f);
            ga[g] = p.getFloat("tc_" + GROUP_KEYS[g] + "_a", 1f);
        }
        if (getWidth() > 0) layoutButtons(getWidth(), getHeight());
        invalidate();
    }

    void saveLayout() {
        SharedPreferences.Editor e = getContext().getSharedPreferences("ttt2", Context.MODE_PRIVATE).edit();
        for (int g = 0; g < 4; g++)
            e.putFloat("tc_" + GROUP_KEYS[g] + "_x", gx[g]).putFloat("tc_" + GROUP_KEYS[g] + "_y", gy[g])
             .putFloat("tc_" + GROUP_KEYS[g] + "_s", gs[g]).putFloat("tc_" + GROUP_KEYS[g] + "_a", ga[g]);
        e.apply();
    }

    void resetLayout() {
        for (int g = 0; g < 4; g++) { gx[g] = 0; gy[g] = 0; gs[g] = 1; ga[g] = 1; }
        relayout();
    }

    void relayout() { if (getWidth() > 0) layoutButtons(getWidth(), getHeight()); invalidate(); }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) { layoutButtons(w, h); }

    /** Default positions (as before r23), then each group scaled about its anchor and moved by its offset. */
    private void layoutButtons(int w, int h) {
        float u = Math.min(w, h) / 10f * globalScale;
        // Anchors: the groups' centres in the default layout.
        anchorX[G_STICK] = u * 2.4f; anchorY[G_STICK] = h - u * 2.5f;
        anchorX[G_FACE] = w - u * 2.6f; anchorY[G_FACE] = h - u * 2.6f;
        anchorX[G_SHOULDER] = w - u * 4.3f; anchorY[G_SHOULDER] = h - u * 6.7f;
        anchorX[G_MENU] = w / 2f; anchorY[G_MENU] = u * 0.68f;
        float[] ax = new float[4], ay = new float[4], s = new float[4];
        for (int g = 0; g < 4; g++) { ax[g] = anchorX[g] + gx[g] * w; ay[g] = anchorY[g] + gy[g] * h; s[g] = u * gs[g]; }

        stickR = s[G_STICK] * 1.7f;
        stickCx = ax[G_STICK]; stickCy = ay[G_STICK];
        if (stickPointer < 0) { knobX = stickCx; knobY = stickCy; }
        groupBox[G_STICK].set(stickCx - stickR * 1.25f, stickCy - stickR * 1.25f, stickCx + stickR * 1.25f, stickCy + stickR * 1.25f);

        float fu = s[G_FACE], d = fu * 1.25f, br = fu * 0.62f, fx = ax[G_FACE], fy = ay[G_FACE];
        place(buttons[0], fx, fy + d, br);
        place(buttons[1], fx + d, fy, br);
        place(buttons[2], fx - d, fy, br);
        place(buttons[3], fx, fy - d, br);
        groupBox[G_FACE].set(fx - d - br * 1.3f, fy - d - br * 1.3f, fx + d + br * 1.3f, fy + d + br * 1.3f);

        float su = s[G_SHOULDER], sx = ax[G_SHOULDER], sy = ay[G_SHOULDER], bw = su * 1.8f, bh = su * 0.9f, gap = su * 0.2f;
        box(buttons[4], sx - bw - gap / 2, sy + gap / 2, bw, bh);        // LB
        box(buttons[5], sx + gap / 2, sy + gap / 2, bw, bh);             // RB
        box(buttons[6], sx - bw - gap / 2, sy - bh - gap / 2, bw, bh);   // LT
        box(buttons[7], sx + gap / 2, sy - bh - gap / 2, bw, bh);        // RT
        groupBox[G_SHOULDER].set(sx - bw - gap, sy - bh - gap, sx + bw + gap, sy + bh + gap);

        float mu = s[G_MENU], mw = mu * 1.9f, mh = mu * 0.75f, mx = ax[G_MENU], my = ay[G_MENU];
        box(buttons[9], mx - mw - mu * 0.2f, my - mh / 2, mw, mh);      // BACK
        box(buttons[8], mx + mu * 0.2f, my - mh / 2, mw, mh);           // START
        groupBox[G_MENU].set(mx - mw - mu * 0.4f, my - mh, mx + mw + mu * 0.4f, my + mh);
    }

    private static void place(Btn b, float x, float y, float r) { b.cx = x; b.cy = y; b.r = r; b.rect = false; }
    private static void box(Btn b, float l, float t, float w, float h) { b.rect = true; b.box.set(l, t, l + w, t + h); b.cx = l + w / 2; b.cy = t + h / 2; }

    // ---------------------------------------------------------------- input
    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (editMode) return editTouch(e);
        int action = e.getActionMasked();
        int upIndex = (action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_UP) ? e.getActionIndex() : -1;
        if (action == MotionEvent.ACTION_CANCEL) upIndex = -2;
        int state = 0;
        boolean stickHeld = false;
        float lx = 0, ly = 0;
        for (int i = 0; i < e.getPointerCount(); i++) {
            if (upIndex == -2 || i == upIndex) {
                if (e.getPointerId(i) == stickPointer) stickPointer = -1;
                continue;
            }
            int id = e.getPointerId(i);
            float x = e.getX(i), y = e.getY(i);
            float dx = x - stickCx, dy = y - stickCy;
            boolean inStickZone = dx * dx + dy * dy < stickR * stickR * 2.6f;
            boolean onButton = false;
            for (Btn b : buttons) if (b.hit(x, y)) { onButton = true; break; }
            if (id == stickPointer || (stickPointer < 0 && inStickZone && !onButton)) {
                stickPointer = id;
                stickHeld = true;
                float len = (float) Math.sqrt(dx * dx + dy * dy);
                if (len > stickR) { dx *= stickR / len; dy *= stickR / len; }
                knobX = stickCx + dx; knobY = stickCy + dy;
                lx = dx / stickR; ly = -dy / stickR;
                continue;
            }
            for (Btn b : buttons) if (b.hit(x, y)) state |= b.bit;
        }
        if (!stickHeld) { stickPointer = -1; knobX = stickCx; knobY = stickCy; }
        // 8-way d-pad from the stick (Tekken reads the d-pad for movement).
        float mag = (float) Math.sqrt(lx * lx + ly * ly);
        if (mag > 0.35f) {
            double ang = Math.toDegrees(Math.atan2(ly, lx)); // 0 = right, 90 = up
            if (ang < 0) ang += 360;
            int sector = (int) Math.round(ang / 45.0) % 8;
            switch (sector) {
                case 0: state |= RIGHT; break;
                case 1: state |= RIGHT | UP; break;
                case 2: state |= UP; break;
                case 3: state |= UP | LEFT; break;
                case 4: state |= LEFT; break;
                case 5: state |= LEFT | DOWN; break;
                case 6: state |= DOWN; break;
                case 7: state |= DOWN | RIGHT; break;
            }
        }
        int ilx = Math.round(lx * 32767), ily = Math.round(ly * 32767);
        if (state != lastState || ilx != lastLx || ily != lastLy) {
            if ((state & ~lastState) != 0 && lastState != -1) performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
            lastState = state; lastLx = ilx; lastLy = ily;
            pressed = state;
            try {
                Native.setTouchPad(state & 0xFFFF, ilx, ily, 0, 0, (state & LT) != 0 ? 255 : 0, (state & RT) != 0 ? 255 : 0);
            } catch (Throwable ignored) {}
        }
        postInvalidateOnAnimation();
        return true;
    }

    private boolean editTouch(MotionEvent e) {
        float x = e.getX(), y = e.getY();
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                dragGroup = -1;
                for (int g = 3; g >= 0; g--) if (groupBox[g].contains(x, y)) { dragGroup = g; break; }
                if (dragGroup >= 0) {
                    selected = dragGroup;
                    if (selectionListener != null) selectionListener.selected(selected);
                    dragX = x; dragY = y; dragGx = gx[dragGroup]; dragGy = gy[dragGroup];
                }
                invalidate();
                return true;
            case MotionEvent.ACTION_MOVE:
                if (dragGroup >= 0) {
                    gx[dragGroup] = clamp(dragGx + (x - dragX) / getWidth(), -1f, 1f);
                    gy[dragGroup] = clamp(dragGy + (y - dragY) / getHeight(), -1f, 1f);
                    layoutButtons(getWidth(), getHeight());
                    // Keep the group on screen.
                    RectF b = groupBox[dragGroup];
                    float fixX = b.left < 0 ? -b.left : b.right > getWidth() ? getWidth() - b.right : 0;
                    float fixY = b.top < 0 ? -b.top : b.bottom > getHeight() ? getHeight() - b.bottom : 0;
                    if (fixX != 0 || fixY != 0) { gx[dragGroup] += fixX / getWidth(); gy[dragGroup] += fixY / getHeight(); layoutButtons(getWidth(), getHeight()); }
                    invalidate();
                }
                return true;
            default:
                dragGroup = -1;
                return true;
        }
    }

    private static float clamp(float v, float lo, float hi) { return Math.max(lo, Math.min(hi, v)); }

    // ---------------------------------------------------------------- drawing
    @Override
    protected void onDraw(Canvas c) {
        boolean animating = false;
        // Ease each control toward its state (~150 ms at 60 fps).
        for (Btn b : buttons) {
            float target = (pressed & b.bit) != 0 ? 1f : 0f;
            if (b.press != target) { b.press += (target - b.press) * 0.35f; if (Math.abs(target - b.press) < 0.02f) b.press = target; else animating = true; }
        }
        float st = stickPointer >= 0 ? 1f : 0f;
        if (stickPress != st) { stickPress += (st - stickPress) * 0.3f; if (Math.abs(st - stickPress) < 0.02f) stickPress = st; else animating = true; }

        drawStick(c, ga[G_STICK]);
        for (Btn b : buttons) {
            if (b.rect) drawPill(c, b, ga[b.group]);
            else drawFace(c, b, ga[b.group]);
        }
        if (editMode)
            for (int g = 0; g < 4; g++) {
                dash.setColor(g == selected ? Color.argb(240, 255, 60, 60) : Color.argb(110, 255, 255, 255));
                c.drawRoundRect(groupBox[g], 12 * density, 12 * density, dash);
            }
        if (animating) postInvalidateOnAnimation();
    }

    private static int a(int color, float alpha) { return Color.argb(Math.round(Color.alpha(color) * alpha), Color.red(color), Color.green(color), Color.blue(color)); }
    private static int mix(int c1, int c2, float t) {
        return Color.argb(Math.round(Color.alpha(c1) + (Color.alpha(c2) - Color.alpha(c1)) * t), Math.round(Color.red(c1) + (Color.red(c2) - Color.red(c1)) * t),
            Math.round(Color.green(c1) + (Color.green(c2) - Color.green(c1)) * t), Math.round(Color.blue(c1) + (Color.blue(c2) - Color.blue(c1)) * t));
    }

    private void drawStick(Canvas c, float alpha) {
        float r = stickR;
        // Base: dark glass disc, steel rim, red inner ring that brightens while held.
        fill.setShader(new RadialGradient(stickCx, stickCy - r * 0.3f, r * 1.2f, a(Color.argb(150, 40, 38, 52), alpha), a(Color.argb(120, 8, 8, 14), alpha), Shader.TileMode.CLAMP));
        c.drawCircle(stickCx, stickCy, r, fill);
        fill.setShader(null);
        stroke.setShader(null);
        stroke.setStrokeWidth(2.5f * density);
        stroke.setColor(a(Color.argb(170, 200, 204, 220), alpha));
        c.drawCircle(stickCx, stickCy, r, stroke);
        stroke.setStrokeWidth(2f * density);
        stroke.setColor(a(mix(Color.argb(90, 226, 22, 34), Color.argb(230, 255, 60, 60), stickPress), alpha));
        c.drawCircle(stickCx, stickCy, r * 0.82f, stroke);
        // Direction marks.
        fill.setColor(a(Color.argb(120, 230, 230, 240), alpha));
        for (int i = 0; i < 4; i++) {
            double ang = Math.PI / 2 * i;
            c.drawCircle(stickCx + (float) Math.cos(ang) * r * 0.66f, stickCy + (float) Math.sin(ang) * r * 0.66f, 2.2f * density, fill);
        }
        // Glow toward the knob while held.
        if (stickPress > 0.01f) {
            fill.setColor(a(Color.argb(Math.round(70 * stickPress), 255, 50, 50), alpha));
            c.drawCircle(knobX, knobY, r * 0.62f, fill);
        }
        // Knob: metallic gradient with a red cap ring.
        float kr = r * 0.44f * (1f - 0.06f * stickPress);
        fill.setShader(new LinearGradient(knobX, knobY - kr, knobX, knobY + kr, a(Color.argb(235, 236, 238, 246), alpha), a(Color.argb(235, 110, 114, 132), alpha), Shader.TileMode.CLAMP));
        c.drawCircle(knobX, knobY, kr, fill);
        fill.setShader(null);
        stroke.setStrokeWidth(3f * density);
        stroke.setColor(a(Color.argb(230, 226, 22, 34), alpha));
        c.drawCircle(knobX, knobY, kr * 0.72f, stroke);
    }

    private void drawFace(Canvas c, Btn b, float alpha) {
        float p = b.press, r = b.r * (1f - 0.08f * p);
        int col = b.color;
        // Glow ring opening on press.
        if (p > 0.01f) {
            fill.setColor(a(Color.argb(Math.round(90 * p), Color.red(col), Color.green(col), Color.blue(col)), alpha));
            c.drawCircle(b.cx, b.cy, b.r * (1.15f + 0.35f * p), fill);
        }
        int top = mix(Color.argb(205, Color.red(col), Color.green(col), Color.blue(col)), Color.argb(255, 255, 255, 255), 0.25f + 0.25f * p);
        int bottom = mix(Color.argb(205, Color.red(col) / 3, Color.green(col) / 3, Color.blue(col) / 3), Color.argb(255, Color.red(col), Color.green(col), Color.blue(col)), p);
        fill.setShader(new RadialGradient(b.cx - r * 0.3f, b.cy - r * 0.4f, r * 1.6f, a(top, alpha), a(bottom, alpha), Shader.TileMode.CLAMP));
        c.drawCircle(b.cx, b.cy, r, fill);
        fill.setShader(null);
        // Gloss highlight.
        fill.setColor(a(Color.argb(Math.round(60 - 30 * p), 255, 255, 255), alpha));
        c.drawOval(b.cx - r * 0.62f, b.cy - r * 0.86f, b.cx + r * 0.62f, b.cy - r * 0.12f, fill);
        stroke.setStrokeWidth(2f * density);
        stroke.setColor(a(Color.argb(200, 255, 255, 255), alpha));
        c.drawCircle(b.cx, b.cy, r, stroke);
        text.setTextSize(r * 0.95f);
        text.setColor(a(Color.argb(245, 255, 255, 255), alpha));
        text.setShadowLayer(3 * density, 0, density, Color.argb(Math.round(160 * alpha), 0, 0, 0));
        c.drawText(b.text, b.cx, b.cy - (text.descent() + text.ascent()) / 2, text);
        text.clearShadowLayer();
    }

    private void drawPill(Canvas c, Btn b, float alpha) {
        float p = b.press;
        RectF r = new RectF(b.box);
        float shrink = r.height() * 0.05f * p;
        r.inset(shrink, shrink);
        float rr = r.height() / 2;
        if (p > 0.01f) {
            RectF g = new RectF(r);
            g.inset(-r.height() * 0.22f * p, -r.height() * 0.22f * p);
            fill.setColor(a(Color.argb(Math.round(80 * p), 255, 50, 50), alpha));
            c.drawRoundRect(g, g.height() / 2, g.height() / 2, fill);
        }
        int top = mix(Color.argb(190, 70, 72, 88), Color.argb(235, 240, 60, 60), p);
        int bottom = mix(Color.argb(190, 16, 16, 24), Color.argb(235, 140, 10, 20), p);
        fill.setShader(new LinearGradient(r.left, r.top, r.left, r.bottom, a(top, alpha), a(bottom, alpha), Shader.TileMode.CLAMP));
        c.drawRoundRect(r, rr, rr, fill);
        fill.setShader(null);
        stroke.setStrokeWidth(1.8f * density);
        stroke.setColor(a(mix(Color.argb(170, 210, 214, 228), Color.argb(255, 255, 220, 220), p), alpha));
        c.drawRoundRect(r, rr, rr, stroke);
        // Red accent on the left edge.
        fill.setColor(a(Color.argb(220, 226, 22, 34), alpha));
        c.drawCircle(r.left + rr, r.centerY(), rr * 0.18f, fill);
        text.setTextSize(r.height() * 0.42f);
        text.setColor(a(Color.argb(240, 255, 255, 255), alpha));
        c.drawText(b.text, r.centerX() + rr * 0.15f, r.centerY() - (text.descent() + text.ascent()) / 2, text);
    }
}
