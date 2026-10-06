package dev.thumb.overlay;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Application;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
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
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

import org.json.JSONObject;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.Locale;
import java.util.WeakHashMap;

/**
 * THUMB's in-game menu. Added to patched apps as an extra dex and started by
 * the THUMB runtime (libthumb.so), which also provides the native methods.
 * Plain Android views only: no dependency on anything the app ships.
 */
public final class ThumbOverlay {
    private static native void nativeSetSpeed(float speed);
    private static native float nativeGetFps();
    private static native void nativeSetAdBlock(boolean on);

    private static final int MINT = 0xFF42FFC3;
    private static final int PANEL = 0xEE121614;
    private static final float[] SPEEDS = {0.25f, 0.5f, 0.75f, 1f, 1.25f, 1.5f, 2f, 3f, 4f};

    private static SharedPreferences prefs;
    private static boolean menuEnabled = true;
    // Mods chosen at patch time; each one appears in the menu only if included.
    private static boolean modFps, modSpeed, modFpsUnlock, modAdblock;

    static final String WARN_SPEED = "Changing game speed can break timing-sensitive games, make audio stutter, or cause desyncs in online play.";
    static final String WARN_FPS_UNLOCK = "Many older games tie their game speed to the frame rate. Unlocking FPS may make the game run too fast, " +
        "break physics or animations, or drain more battery. If the game speeds up, use the speed slider to bring it back to 1×.";
    static final String WARN_ADBLOCK = "Blocks calls to known ad SDKs. Some apps may refuse features or crash if their ads can't load.";
    private static final WeakHashMap<Activity, View> buttons = new WeakHashMap<>();
    private static final Handler main = new Handler(Looper.getMainLooper());

    private ThumbOverlay() {}

    /** Called once by the runtime with the patch-time options (JSON). */
    public static void install(final Application app, String optionsJson) {
        JSONObject options;
        try {
            options = new JSONObject(optionsJson == null ? "{}" : optionsJson);
        } catch (Exception e) {
            options = new JSONObject();
        }
        menuEnabled = options.optBoolean("overlay", true);
        JSONObject mods = options.optJSONObject("mods");
        if (mods == null) mods = new JSONObject();
        modFps = mods.optBoolean("fps_counter", true);
        modSpeed = mods.optBoolean("speed", true);
        modFpsUnlock = mods.optBoolean("fps_unlock", true);
        modAdblock = mods.optBoolean("adblock", true);
        prefs = app.getSharedPreferences("thumb_overlay", 0);
        // Live changes from the menu override the patch-time defaults.
        float speed = prefs.getFloat("speed", (float) options.optDouble("speed", 1.0));
        // Ad blocking is a build option (applied natively at startup); the menu
        // toggle, if included, can override it while playing.
        boolean adblock = prefs.getBoolean("adblock", options.optBoolean("adblock", true));
        if (!prefs.contains("fps_unlock")) prefs.edit().putBoolean("fps_unlock", options.optBoolean("fps_unlock", false)).apply();
        if (modSpeed && speed != 1f) nativeSetSpeed(speed);
        if (modAdblock) nativeSetAdBlock(adblock);

        main.post(new Runnable() {
            @Override public void run() {
                app.registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
                    @Override public void onActivityResumed(Activity a) { attach(a); }
                    @Override public void onActivityCreated(Activity a, Bundle b) {}
                    @Override public void onActivityStarted(Activity a) {}
                    @Override public void onActivityPaused(Activity a) {}
                    @Override public void onActivityStopped(Activity a) {}
                    @Override public void onActivitySaveInstanceState(Activity a, Bundle b) {}
                    @Override public void onActivityDestroyed(Activity a) { buttons.remove(a); }
                });
            }
        });
    }

    private static int dp(Activity a, float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, a.getResources().getDisplayMetrics());
    }

    private static void attach(final Activity a) {
        if (modFpsUnlock) applyFpsUnlock(a, prefs.getBoolean("fps_unlock", false));
        if (!menuEnabled || buttons.containsKey(a)) return;
        Window w = a.getWindow();
        if (w == null || !(w.getDecorView() instanceof ViewGroup)) return;
        hookGestures(a, w);

        TextView button = new TextView(a);
        button.setText("👍"); // thumbs up
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        button.setGravity(Gravity.CENTER);
        GradientDrawable bg = new GradientDrawable();
        bg.setShape(GradientDrawable.OVAL);
        bg.setColor(MINT);
        button.setBackground(bg);
        button.setAlpha(0.8f);
        int size = dp(a, 44);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(size, size, Gravity.TOP | Gravity.START);
        lp.leftMargin = prefs.getInt("button_x", dp(a, 12));
        lp.topMargin = prefs.getInt("button_y", dp(a, 64));
        button.setVisibility(prefs.getBoolean("hidden", false) ? View.GONE : View.VISIBLE);
        makeDraggable(a, button);
        ((ViewGroup) w.getDecorView()).addView(button, lp);
        buttons.put(a, button);
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

        final TextView fps = text(a, "FPS: …", 15, Color.WHITE);
        if (modFps) panel.addView(fps);
        final Runnable tick = new Runnable() {
            @Override public void run() {
                fps.setText(String.format(Locale.ROOT, "FPS: %.0f", nativeGetFps()));
                if (fps.isAttachedToWindow()) main.postDelayed(this, 500);
            }
        };
        main.postDelayed(tick, 300);

        // Speed
        float speed = prefs.getFloat("speed", 1f);
        int index = 3;
        for (int i = 0; i < SPEEDS.length; i++) if (Math.abs(SPEEDS[i] - speed) < 0.01f) index = i;
        final TextView speedLabel = text(a, "Speed: " + fmt(SPEEDS[index]), 15, Color.WHITE);
        speedLabel.setPadding(0, dp(a, 12), 0, 0);
        final SeekBar bar = new SeekBar(a);
        bar.setMax(SPEEDS.length - 1);
        bar.setProgress(index);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean user) {
                speedLabel.setText("Speed: " + fmt(SPEEDS[p]));
                nativeSetSpeed(SPEEDS[p]);
                prefs.edit().putFloat("speed", SPEEDS[p]).apply();
            }
            @Override public void onStartTrackingTouch(SeekBar s) {
                // First change: explain what speed does to a game.
                if (!prefs.getBoolean("speed_warned", false)) {
                    prefs.edit().putBoolean("speed_warned", true).apply();
                    new AlertDialog.Builder(a).setTitle("Change game speed?").setMessage(WARN_SPEED)
                        .setPositiveButton("OK", null).show();
                }
            }
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        if (modSpeed) {
            panel.addView(speedLabel);
            panel.addView(bar);
        }

        // FPS unlock (with the warning the first time)
        final Switch unlock = new Switch(a);
        unlock.setText("Unlock FPS (high refresh rate)");
        unlock.setTextColor(Color.WHITE);
        unlock.setChecked(prefs.getBoolean("fps_unlock", false));
        unlock.setOnCheckedChangeListener((sw, on) -> {
            if (on && !prefs.getBoolean("fps_warned", false)) {
                new AlertDialog.Builder(a)
                    .setTitle("Unlock FPS?")
                    .setMessage(WARN_FPS_UNLOCK)
                    .setPositiveButton("Unlock", (d, w) -> {
                        prefs.edit().putBoolean("fps_warned", true).putBoolean("fps_unlock", true).apply();
                        applyFpsUnlock(a, true);
                    })
                    .setNegativeButton("Cancel", (d, w) -> unlock.setChecked(false))
                    .setOnCancelListener(d -> unlock.setChecked(false))
                    .show();
                return;
            }
            prefs.edit().putBoolean("fps_unlock", on).apply();
            applyFpsUnlock(a, on);
        });
        if (modFpsUnlock) panel.addView(unlock);

        // Ad block
        Switch ads = new Switch(a);
        ads.setText("Block ads");
        ads.setTextColor(Color.WHITE);
        ads.setChecked(prefs.getBoolean("adblock", true));
        ads.setOnCheckedChangeListener((sw, on) -> {
            prefs.edit().putBoolean("adblock", on).apply();
            nativeSetAdBlock(on);
        });
        if (modAdblock) panel.addView(ads);

        TextView hint = text(a, "Hide button: three-finger double tap brings it back.", 12, 0xFF9AA8A2);
        hint.setPadding(0, dp(a, 12), 0, 0);
        panel.addView(hint);

        final AlertDialog dialog = new AlertDialog.Builder(a).setView(panel).create();
        LinearLayout row = new LinearLayout(a);
        row.setPadding(0, dp(a, 8), 0, 0);
        TextView hide = button(a, "HIDE BUTTON");
        hide.setOnClickListener(v -> { setHidden(a, true); dialog.dismiss(); });
        TextView close = button(a, "CLOSE");
        close.setOnClickListener(v -> dialog.dismiss());
        row.addView(hide);
        row.addView(close);
        panel.addView(row);
        dialog.show();
        if (dialog.getWindow() != null) dialog.getWindow().setBackgroundDrawableResource(android.R.color.transparent);
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

    private static String fmt(float s) {
        return (s == (int) s ? String.valueOf((int) s) : String.valueOf(s)) + "×";
    }

    private static void setHidden(Activity a, boolean hidden) {
        prefs.edit().putBoolean("hidden", hidden).apply();
        View b = buttons.get(a);
        if (b != null) b.setVisibility(hidden ? View.GONE : View.VISIBLE);
    }

    /** Ask for the display's highest refresh rate at the current resolution, or the default. */
    private static void applyFpsUnlock(Activity a, boolean on) {
        Window w = a.getWindow();
        if (w == null) return;
        WindowManager.LayoutParams lp = w.getAttributes();
        int modeId = 0;
        if (on) {
            Display d = a.getWindowManager().getDefaultDisplay();
            Display.Mode current = d.getMode();
            float best = 0;
            for (Display.Mode m : d.getSupportedModes()) {
                if (m.getPhysicalWidth() == current.getPhysicalWidth() && m.getPhysicalHeight() == current.getPhysicalHeight()
                        && m.getRefreshRate() > best) {
                    best = m.getRefreshRate();
                    modeId = m.getModeId();
                }
            }
        }
        lp.preferredDisplayModeId = modeId;
        w.setAttributes(lp);
    }

    /**
     * Three-finger double tap toggles the button. The window's touch callback
     * is wrapped so the gesture works anywhere in the game.
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
