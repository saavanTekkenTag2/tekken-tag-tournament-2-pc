package com.saavan.ttt2;

import android.content.Context;
import android.content.SharedPreferences;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;

/**
 * r29: the phone's GPU name (read once with a tiny offscreen OpenGL ES context, then remembered), so the launcher can
 * pick settings that suit it: Mali GPUs get lighter defaults and a 540p limit.
 */
final class GpuInfo {
    private GpuInfo() {}

    static String renderer(Context c) {
        SharedPreferences p = c.getSharedPreferences("ttt2", Context.MODE_PRIVATE);
        String cached = p.getString("gpu_renderer", null);
        if (cached != null) return cached;
        String r = query();
        p.edit().putString("gpu_renderer", r).apply();
        return r;
    }

    static boolean isMali(Context c) {
        return renderer(c).toLowerCase(java.util.Locale.US).contains("mali");
    }

    private static String query() {
        EGLDisplay d = EGL14.EGL_NO_DISPLAY;
        EGLContext ctx = EGL14.EGL_NO_CONTEXT;
        EGLSurface s = EGL14.EGL_NO_SURFACE;
        try {
            d = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
            int[] v = new int[2];
            if (!EGL14.eglInitialize(d, v, 0, v, 1)) return "";
            int[] attr = { EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE };
            EGLConfig[] cfg = new EGLConfig[1];
            int[] n = new int[1];
            if (!EGL14.eglChooseConfig(d, attr, 0, cfg, 0, 1, n, 0) || n[0] == 0) return "";
            ctx = EGL14.eglCreateContext(d, cfg[0], EGL14.EGL_NO_CONTEXT, new int[] { EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE }, 0);
            s = EGL14.eglCreatePbufferSurface(d, cfg[0], new int[] { EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE }, 0);
            if (!EGL14.eglMakeCurrent(d, s, s, ctx)) return "";
            String r = GLES20.glGetString(GLES20.GL_RENDERER);
            return r == null ? "" : r;
        } catch (RuntimeException e) {
            return "";
        } finally {
            if (d != EGL14.EGL_NO_DISPLAY) {
                EGL14.eglMakeCurrent(d, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                if (s != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(d, s);
                if (ctx != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(d, ctx);
                EGL14.eglTerminate(d);
            }
        }
    }
}
