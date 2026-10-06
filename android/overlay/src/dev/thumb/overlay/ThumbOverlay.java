package dev.thumb.overlay;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Application;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.InputStream;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.Locale;
import java.util.WeakHashMap;

/**
 * THUMB's in-game menu. Added to patched apps as an extra dex and started by
 * the THUMB runtime (libthumb.so), which also provides the native methods.
 * Plain Android views only: no dependency on anything the app ships.
 *
 * Every mod is optional (chosen in the THUMB app when patching) and appears in
 * the menu only if it was included. Choices made in the menu are remembered.
 * Hide and Restart are always there.
 */
public final class ThumbOverlay {
    private static native void nativeSetSpeed(float speed);
    private static native float nativeGetFps();
    private static native void nativeSetAdBlock(boolean on);
    private static native void nativeSetFpsLimit(int fps);

    private static final int MINT = 0xFF42FFC3;
    private static final int PANEL = 0xEE121614;
    private static final int MUTED = 0xFF9AA8A2;
    private static final float[] SPEEDS = {0.25f, 0.5f, 0.75f, 1f, 1.25f, 1.5f, 2f, 3f, 4f};
    // The FPS build option: "compat" (60 FPS) or "default" (the screen decides).
    private static String fpsModeDefault = "compat";

    static final String WARN_SPEED = "Changing game speed can break timing-sensitive games, make audio stutter, or cause desyncs in online play.";
    static final String WARN_FPS_HIGH = "Most legacy games were built for 60 FPS. Running them faster can break physics, animations or game speed (for example characters moving or jumping too fast).";

    private static SharedPreferences prefs;
    private static Application app;
    private static boolean menuEnabled;
    private static boolean adblockDefault = true; // the "Block ads" build option
    private static boolean modFps, modSpeed, modFpsLimit, modAdblock, modScreenOn;
    private static Bitmap icon;
    private static final WeakHashMap<Activity, View> buttons = new WeakHashMap<>();
    private static final WeakHashMap<Activity, TextView> badges = new WeakHashMap<>();
    private static final Handler main = new Handler(Looper.getMainLooper());

    private ThumbOverlay() {}

    /** Called once by the runtime with the patch-time options (JSON). */
    public static void install(final Application application, String optionsJson) {
        app = application;
        JSONObject options;
        try {
            options = new JSONObject(optionsJson == null ? "{}" : optionsJson);
        } catch (Exception e) {
            options = new JSONObject();
        }
        menuEnabled = options.optBoolean("overlay", false);
        adblockDefault = options.optBoolean("adblock", true);
        fpsModeDefault = options.optString("fps_mode", "compat");
        JSONObject mods = options.optJSONObject("mods");
        if (mods == null) mods = new JSONObject();
        modFps = mods.optBoolean("fps_counter");
        modSpeed = mods.optBoolean("speed");
        modFpsLimit = mods.optBoolean("fps_unlock");
        modAdblock = mods.optBoolean("adblock");
        modScreenOn = mods.optBoolean("keep_screen_on");
        prefs = app.getSharedPreferences("thumb_overlay", 0);
        icon = loadIcon();

        // Live changes from the menu override the patch-time defaults.
        if (modSpeed) {
            float speed = prefs.getFloat("speed", 1f);
            if (speed != 1f) nativeSetSpeed(speed);
        }
        if (modAdblock && prefs.contains("adblock")) nativeSetAdBlock(prefs.getBoolean("adblock", true));

        main.post(new Runnable() {
            @Override public void run() {
                app.registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
                    @Override public void onActivityResumed(Activity a) { attach(a); }
                    @Override public void onActivityCreated(Activity a, Bundle b) {}
                    @Override public void onActivityStarted(Activity a) {}
                    @Override public void onActivityPaused(Activity a) {}
                    @Override public void onActivityStopped(Activity a) {}
                    @Override public void onActivitySaveInstanceState(Activity a, Bundle b) {}
                    @Override public void onActivityDestroyed(Activity a) {
                        buttons.remove(a);
                        badges.remove(a);
                    }
                });
            }
        });
        main.post(new Runnable() { // keep FPS badges fresh
            @Override public void run() {
                if (!badges.isEmpty()) {
                    String text = String.format(Locale.ROOT, "%.0f FPS", nativeGetFps());
                    for (TextView b : badges.values()) b.setText(text);
                }
                main.postDelayed(this, 500);
            }
        });
    }

    private static Bitmap loadIcon() {
        try (InputStream in = app.getAssets().open("thumb/icon.png")) {
            return BitmapFactory.decodeStream(in);
        } catch (Exception e) {
            return null;
        }
    }

    private static int dp(Activity a, float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, a.getResources().getDisplayMetrics());
    }

    // ---------------------------------------------------------------- attach

    private static void attach(final Activity a) {
        Window w = a.getWindow();
        if (w == null || !(w.getDecorView() instanceof ViewGroup)) return;
        ViewGroup decor = (ViewGroup) w.getDecorView();

        // Persistent mod states, re-applied to every screen of the app.
        applyFps(a, fpsTarget()); // build option, or the menu's FPS unlock
        if (modScreenOn) applyScreenOn(a, prefs.getBoolean("screen_on", false));

        if (modFps && prefs.getBoolean("fps_badge", false)) addBadge(a, decor);

        if (!menuEnabled || buttons.containsKey(a)) return;
        hookGestures(a, w);
        View button;
        if (icon != null) {
            ImageView iv = new ImageView(a);
            iv.setImageBitmap(icon);
            iv.setScaleType(ImageView.ScaleType.FIT_CENTER);
            button = iv;
        } else {
            TextView tv = new TextView(a);
            tv.setText("T");
            tv.setGravity(Gravity.CENTER);
            tv.setTextColor(Color.BLACK);
            GradientDrawable bg = new GradientDrawable();
            bg.setColor(MINT);
            bg.setCornerRadius(dp(a, 8));
            tv.setBackground(bg);
            button = tv;
        }
        button.setAlpha(0.85f);
        button.setElevation(dp(a, 6));
        int size = dp(a, 46);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(size, size, Gravity.TOP | Gravity.START);
        lp.leftMargin = prefs.getInt("button_x", dp(a, 12));
        lp.topMargin = prefs.getInt("button_y", dp(a, 64));
        button.setVisibility(prefs.getBoolean("hidden", false) ? View.GONE : View.VISIBLE);
        makeDraggable(a, button);
        decor.addView(button, lp);
        buttons.put(a, button);
    }

    /** The on-screen FPS counter (top right). */
    private static void addBadge(Activity a, ViewGroup decor) {
        if (badges.containsKey(a)) return;
        TextView badge = new TextView(a);
        badge.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        badge.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        badge.setTextColor(MINT);
        badge.setBackgroundColor(0x99000000);
        int p = dp(a, 4);
        badge.setPadding(p * 2, p, p * 2, p);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.END);
        lp.topMargin = dp(a, 28);
        lp.rightMargin = dp(a, 8);
        decor.addView(badge, lp);
        badges.put(a, badge);
    }

    private static void setBadge(Activity a, boolean on) {
        if (on) {
            addBadge(a, (ViewGroup) a.getWindow().getDecorView());
        } else {
            TextView b = badges.remove(a);
            if (b != null && b.getParent() instanceof ViewGroup) ((ViewGroup) b.getParent()).removeView(b);
        }
    }

    /** Drag to move; a tap without movement opens the menu. */
    private static void makeDraggable(final Activity a, final View v) {
        v.setOnTouchListener(new View.OnTouchListener() {
            float downX, downY;
            int startLeft, startTop;
            boolean moved;

            @Override public boolean onTouch(View view, MotionEvent e) {
                FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) view.getLayoutParams();
                switch (e.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        downX = e.getRawX(); downY = e.getRawY();
                        startLeft = lp.leftMargin; startTop = lp.topMargin;
                        moved = false;
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        float dx = e.getRawX() - downX, dy = e.getRawY() - downY;
                        if (Math.abs(dx) + Math.abs(dy) > dp(a, 6)) moved = true;
                        if (moved) {
                            lp.leftMargin = Math.max(0, startLeft + (int) dx);
                            lp.topMargin = Math.max(0, startTop + (int) dy);
                            view.setLayoutParams(lp);
                        }
                        return true;
                    case MotionEvent.ACTION_UP:
                        if (moved) prefs.edit().putInt("button_x", lp.leftMargin).putInt("button_y", lp.topMargin).apply();
                        else showMenu(a);
                        return true;
                    default:
                        return true;
                }
            }
        });
    }

    // ------------------------------------------------------------------ menu

    private static void showMenu(final Activity a) {
        LinearLayout panel = new LinearLayout(a);
        panel.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(a, 18);
        panel.setPadding(pad, pad, pad, pad);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(PANEL);
        bg.setCornerRadius(dp(a, 16));
        panel.setBackground(bg);

        TextView title = text(a, "THUMB", 22, MINT);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        panel.addView(title);

        if (modFps) {
            final TextView fps = text(a, "FPS: …", 15, Color.WHITE);
            panel.addView(fps);
            main.postDelayed(new Runnable() {
                @Override public void run() {
                    fps.setText(String.format(Locale.ROOT, "FPS: %.0f", nativeGetFps()));
                    if (fps.isAttachedToWindow()) main.postDelayed(this, 500);
                }
            }, 300);
            panel.addView(toggle(a, "Show FPS on screen", "fps_badge", false, null, null, on -> setBadge(a, on)));
        }

        if (modSpeed) addSpeed(a, panel);

        if (modFpsLimit) addFpsLimit(a, panel);
        if (modAdblock) panel.addView(toggle(a, "Block ads", "adblock", adblockDefault, null, null, ThumbOverlay::nativeSetAdBlock));
        if (modScreenOn) panel.addView(toggle(a, "Keep screen on", "screen_on", false, null, null, on -> applyScreenOn(a, on)));

        TextView hint = text(a, "Three-finger double tap hides or shows this button.", 12, MUTED);
        hint.setPadding(0, dp(a, 12), 0, 0);
        panel.addView(hint);

        ScrollView scroll = new ScrollView(a);
        scroll.addView(panel);
        final AlertDialog dialog = new AlertDialog.Builder(a).setView(scroll).create();

        // Bottom: HIDE / CLOSE on the left, FORCE RESTART / KILL (for frozen apps) on the right.
        TextView warn = text(a, "⚠️ Force restart and Kill close the app without saving. Use them only if it's frozen or misbehaving.", 12, 0xFFFFC107);
        warn.setPadding(0, dp(a, 12), 0, 0);
        panel.addView(warn);
        TextView hide = button(a, "HIDE");
        hide.setOnClickListener(v -> { setHidden(a, true); dialog.dismiss(); });
        TextView close = button(a, "CLOSE");
        close.setOnClickListener(v -> dialog.dismiss());
        TextView restart = button(a, "FORCE RESTART");
        restart.setOnClickListener(v -> new AlertDialog.Builder(a)
            .setTitle("Force restart the app?")
            .setMessage("Unsaved progress will be lost.")
            .setPositiveButton("Restart", (d, w) -> restartApp(a))
            .setNegativeButton("Cancel", null)
            .show());
        TextView kill = button(a, "KILL");
        kill.setTextColor(0xFFFF8A80);
        kill.setOnClickListener(v -> new AlertDialog.Builder(a)
            .setTitle("Kill the app?")
            .setMessage("Closes it immediately. Unsaved progress will be lost.")
            .setPositiveButton("Kill", (d, w) -> killApp(a))
            .setNegativeButton("Cancel", null)
            .show());
        panel.addView(buttonRow(a, hide, restart));
        panel.addView(buttonRow(a, close, kill));

        dialog.show();
        if (dialog.getWindow() != null) dialog.getWindow().setBackgroundDrawableResource(android.R.color.transparent);
    }

    private interface OnToggle { void apply(boolean on); }

    /** A remembered switch; [warnTitle]/[warning] are shown once, the first time it's turned on. */
    private static Switch toggle(final Activity a, String label, final String key, boolean def,
                                 final String warnTitle, final String warning, final OnToggle onToggle) {
        final Switch sw = new Switch(a);
        sw.setText(label);
        sw.setTextColor(Color.WHITE);
        sw.setPadding(0, dp(a, 6), 0, dp(a, 6));
        sw.setChecked(prefs.getBoolean(key, def));
        sw.setOnCheckedChangeListener((s, on) -> {
            if (on && warning != null && !prefs.getBoolean(key + "_warned", false)) {
                new AlertDialog.Builder(a).setTitle(warnTitle).setMessage(warning)
                    .setPositiveButton("Turn on", (d, w) -> {
                        prefs.edit().putBoolean(key + "_warned", true).putBoolean(key, true).apply();
                        onToggle.apply(true);
                    })
                    .setNegativeButton("Cancel", (d, w) -> sw.setChecked(false))
                    .setOnCancelListener(d -> sw.setChecked(false))
                    .show();
                return;
            }
            prefs.edit().putBoolean(key, on).apply();
            onToggle.apply(on);
        });
        return sw;
    }

    private static void addSpeed(final Activity a, LinearLayout panel) {
        float speed = prefs.getFloat("speed", 1f);
        int index = 3;
        for (int i = 0; i < SPEEDS.length; i++) if (Math.abs(SPEEDS[i] - speed) < 0.01f) index = i;
        final TextView label = text(a, "Speed: " + fmt(SPEEDS[index]), 15, Color.WHITE);
        label.setPadding(0, dp(a, 12), 0, 0);
        SeekBar bar = new SeekBar(a);
        bar.setMax(SPEEDS.length - 1);
        bar.setProgress(index);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean user) {
                label.setText("Speed: " + fmt(SPEEDS[p]));
                nativeSetSpeed(SPEEDS[p]);
                prefs.edit().putFloat("speed", SPEEDS[p]).apply();
            }
            @Override public void onStartTrackingTouch(SeekBar s) {
                if (!prefs.getBoolean("speed_warned", false)) {
                    prefs.edit().putBoolean("speed_warned", true).apply();
                    new AlertDialog.Builder(a).setTitle("Change game speed?").setMessage(WARN_SPEED).setPositiveButton("OK", null).show();
                }
            }
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        panel.addView(label);
        panel.addView(bar);
    }

    /**
     * FPS unlock: off = the build option (Compat 60 or Default); on = a slider
     * from 30 to the screen's highest refresh rate, the last step being "Max"
     * (no cap). Turning it on warns once about legacy game physics.
     */
    private static void addFpsLimit(final Activity a, LinearLayout panel) {
        final int top = maxRefresh(a);
        final int steps = Math.max(1, (top - 30) / 10 + 1); // 30, 40, ... top-ish, then Max
        final TextView label = text(a, "", 15, Color.WHITE);
        label.setPadding(dp(a, 4), 0, 0, 0);
        final SeekBar bar = new SeekBar(a);
        bar.setMax(steps);
        final Runnable refresh = () -> {
            boolean on = prefs.getBoolean("fps_unlock", false);
            int v = prefs.getInt("fps_value", 60);
            bar.setVisibility(on ? View.VISIBLE : View.GONE);
            label.setText(on ? (v == 0 ? "FPS: Max (" + top + " Hz screen)" : "FPS: " + v)
                : "compat".equals(fpsModeDefault) ? "FPS: 60 (Compat)" : "FPS: screen default");
        };
        int v = prefs.getInt("fps_value", 60);
        bar.setProgress(v == 0 ? steps : Math.max(0, Math.min(steps - 1, (v - 30) / 10)));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean user) {
                if (!user) return;
                prefs.edit().putInt("fps_value", p == steps ? 0 : 30 + p * 10).apply();
                applyFps(a, fpsTarget());
                refresh.run();
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        panel.addView(toggle(a, "FPS unlock", "fps_unlock", false, "Unlock FPS?", WARN_FPS_HIGH, on -> {
            applyFps(a, fpsTarget());
            refresh.run();
        }));
        panel.addView(label);
        panel.addView(bar);
        refresh.run();
    }

    private static TextView text(Activity a, String s, float sp, int color) {
        TextView t = new TextView(a);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        return t;
    }

    private static TextView button(Activity a, String s) {
        TextView t = text(a, s, 14, MINT);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        int p = dp(a, 10);
        t.setPadding(p, p, p * 2, p);
        return t;
    }

    /** A row with [left] at the start and [right] at the end. */
    private static LinearLayout buttonRow(Activity a, View left, View right) {
        LinearLayout row = new LinearLayout(a);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.addView(left);
        View gap = new View(a);
        row.addView(gap, new LinearLayout.LayoutParams(0, 1, 1f));
        row.addView(right);
        return row;
    }

    private static String fmt(float s) {
        return (s == (int) s ? String.valueOf((int) s) : String.valueOf(s)) + "×";
    }

    private static void setHidden(Activity a, boolean hidden) {
        prefs.edit().putBoolean("hidden", hidden).apply();
        View b = buttons.get(a);
        if (b != null) b.setVisibility(hidden ? View.GONE : View.VISIBLE);
    }

    // ------------------------------------------------------------------ mods

    /** Frame-rate target: -1 = untouched (screen decides), 0 = max, n = cap at n. */
    private static int fpsTarget() {
        if (modFpsLimit && prefs.getBoolean("fps_unlock", false)) return prefs.getInt("fps_value", 60);
        return "compat".equals(fpsModeDefault) ? 60 : -1;
    }

    private static int maxRefresh(Activity a) {
        float best = 60;
        for (Display.Mode m : a.getWindowManager().getDefaultDisplay().getSupportedModes()) best = Math.max(best, m.getRefreshRate());
        return Math.round(best);
    }

    /**
     * Picks a display refresh rate for [target] (the lowest one at or above a
     * cap, saving battery; the highest for max) and tells the runtime to pace
     * frames at the cap, so in-between values like 45 or 90 are exact.
     */
    private static void applyFps(Activity a, int target) {
        nativeSetFpsLimit(Math.max(target, 0));
        Window w = a.getWindow();
        if (w == null) return;
        WindowManager.LayoutParams lp = w.getAttributes();
        int modeId = 0;
        if (target >= 0) {
            Display d = a.getWindowManager().getDefaultDisplay();
            Display.Mode current = d.getMode();
            float best = target > 0 ? Float.MAX_VALUE : 0;
            for (Display.Mode m : d.getSupportedModes()) {
                if (m.getPhysicalWidth() != current.getPhysicalWidth() || m.getPhysicalHeight() != current.getPhysicalHeight()) continue;
                float r = m.getRefreshRate();
                if (target > 0 ? (r >= target - 1 && r < best) : r > best) {
                    best = r;
                    modeId = m.getModeId();
                }
            }
        }
        lp.preferredDisplayModeId = modeId;
        w.setAttributes(lp);
    }

    private static void applyScreenOn(Activity a, boolean on) {
        if (on) a.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else a.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    /** Closes the app right away (also removes it from recents). */
    private static void killApp(Activity a) {
        a.finishAndRemoveTask();
        main.postDelayed(() -> android.os.Process.killProcess(android.os.Process.myPid()), 150);
    }

    /** Relaunches the app from scratch. */
    private static void restartApp(Activity a) {
        Intent launch = a.getPackageManager().getLaunchIntentForPackage(a.getPackageName());
        if (launch != null) {
            launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            a.startActivity(launch);
        }
        a.finishAffinity();
        main.postDelayed(() -> android.os.Process.killProcess(android.os.Process.myPid()), 300);
    }

    /**
     * Three-finger double tap toggles the button. The window's touch callback
     * is wrapped so the gesture works anywhere in the app.
     */
    private static void hookGestures(final Activity a, final Window w) {
        final Window.Callback original = w.getCallback();
        if (original == null || Proxy.isProxyClass(original.getClass())) return;
        final long[] lastTap = {0};
        Object proxy = Proxy.newProxyInstance(Window.Callback.class.getClassLoader(), new Class<?>[] {Window.Callback.class},
            new InvocationHandler() {
                @Override public Object invoke(Object p, Method m, Object[] args) throws Throwable {
                    if ("dispatchTouchEvent".equals(m.getName()) && args != null && args[0] instanceof MotionEvent) {
                        MotionEvent e = (MotionEvent) args[0];
                        if (e.getActionMasked() == MotionEvent.ACTION_POINTER_DOWN && e.getPointerCount() == 3) {
                            long now = e.getEventTime();
                            if (now - lastTap[0] < 400) {
                                lastTap[0] = 0;
                                setHidden(a, !prefs.getBoolean("hidden", false));
                            } else {
                                lastTap[0] = now;
                            }
                        }
                    }
                    return m.invoke(original, args);
                }
            });
        w.setCallback((Window.Callback) proxy);
    }
}
