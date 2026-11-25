package com.saavan.ttt2;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/**
 * On-screen pad drawn over the game. Left: joystick (d-pad bits + left stick). Right: face
 * buttons in the Xbox diamond (Y top, A bottom, X left, B right), shoulders and triggers above.
 * Start/Back at the top middle. Fed to XInput slot 0 through Native.setTouchPad.
 */
public class TouchOverlay extends View {
    static final int UP = 0x1, DOWN = 0x2, LEFT = 0x4, RIGHT = 0x8, START = 0x10, BACK = 0x20,
        LB = 0x100, RB = 0x200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
    static final int LT = 0x10000, RT = 0x20000; // overlay-only bits, sent as trigger values

    private static final class Btn {
        final int bit; final String text; float cx, cy, r; boolean rect; RectF box = new RectF();
        Btn(int bit, String text) { this.bit = bit; this.text = text; }
        boolean hit(float x, float y) {
            if (rect) return box.contains(x, y);
            float dx = x - cx, dy = y - cy; return dx * dx + dy * dy <= r * r * 1.35f;
        }
    }

    private final Btn[] buttons = {
        new Btn(A, "A"), new Btn(B, "B"), new Btn(X, "X"), new Btn(Y, "Y"),
        new Btn(LB, "LB"), new Btn(RB, "RB"), new Btn(LT, "LT"), new Btn(RT, "RT"),
        new Btn(START, "START"), new Btn(BACK, "BACK"),
    };
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), ring = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private float stickCx, stickCy, stickR;
    private int stickPointer = -1;
    private float knobX, knobY;
    private int lastState = -1, lastLx, lastLy;

    public TouchOverlay(Context c) {
        super(c);
        ring.setStyle(Paint.Style.STROKE);
        ring.setStrokeWidth(3f * getResources().getDisplayMetrics().density);
        ring.setColor(Color.argb(150, 255, 255, 255));
        text.setColor(Color.argb(200, 255, 255, 255));
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        setFocusable(false);
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        float u = Math.min(w, h) / 10f;
        stickR = u * 1.7f;
        stickCx = u * 2.4f; stickCy = h - u * 2.5f;
        knobX = stickCx; knobY = stickCy;
        float fx = w - u * 2.6f, fy = h - u * 2.6f, d = u * 1.25f, br = u * 0.62f;
        place(buttons[0], fx, fy + d, br);      // A
        place(buttons[1], fx + d, fy, br);      // B
        place(buttons[2], fx - d, fy, br);      // X
        place(buttons[3], fx, fy - d, br);      // Y
        box(buttons[4], w - u * 7.2f, h - u * 6.6f, u * 1.8f, u * 0.9f);   // LB
        box(buttons[5], w - u * 5.2f, h - u * 6.6f, u * 1.8f, u * 0.9f);   // RB
        box(buttons[6], w - u * 7.2f, h - u * 7.7f, u * 1.8f, u * 0.9f);   // LT
        box(buttons[7], w - u * 5.2f, h - u * 7.7f, u * 1.8f, u * 0.9f);   // RT
        box(buttons[8], w / 2f + u * 0.2f, u * 0.3f, u * 1.9f, u * 0.75f); // START
        box(buttons[9], w / 2f - u * 2.1f, u * 0.3f, u * 1.9f, u * 0.75f); // BACK
        text.setTextSize(u * 0.42f);
    }

    private static void place(Btn b, float x, float y, float r) { b.cx = x; b.cy = y; b.r = r; b.rect = false; }
    private static void box(Btn b, float l, float t, float w, float h) { b.rect = true; b.box.set(l, t, l + w, t + h); b.cx = l + w / 2; b.cy = t + h / 2; }

    private int pressed;

    @Override
    public boolean onTouchEvent(MotionEvent e) {
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
            boolean inStickZone = x < getWidth() * 0.42f && y > getHeight() * 0.3f;
            if (id == stickPointer || (stickPointer < 0 && inStickZone && dx * dx + dy * dy < stickR * stickR * 2.6f)) {
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
        invalidate();
        return true;
    }

    @Override
    protected void onDraw(Canvas c) {
        fill.setColor(Color.argb(55, 255, 255, 255));
        c.drawCircle(stickCx, stickCy, stickR, fill);
        c.drawCircle(stickCx, stickCy, stickR, ring);
        fill.setColor(Color.argb(stickPointer >= 0 ? 150 : 90, 255, 255, 255));
        c.drawCircle(knobX, knobY, stickR * 0.42f, fill);
        float th = (text.descent() + text.ascent()) / 2;
        for (Btn b : buttons) {
            boolean on = (pressed & b.bit) != 0;
            fill.setColor(on ? Color.argb(170, 255, 150, 40) : Color.argb(60, 255, 255, 255));
            if (b.rect) {
                float rr = b.box.height() / 2;
                c.drawRoundRect(b.box, rr, rr, fill);
                c.drawRoundRect(b.box, rr, rr, ring);
            } else {
                c.drawCircle(b.cx, b.cy, b.r, fill);
                c.drawCircle(b.cx, b.cy, b.r, ring);
            }
            c.drawText(b.text, b.cx, b.cy - th, text);
        }
    }
}
