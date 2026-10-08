package dev.thumb.overlay;

import android.app.Application;
import android.media.AudioTrack;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.json.JSONArray;

import java.lang.ref.WeakReference;
import java.lang.reflect.Field;

/**
 * Per-game fixes (like Proton's): small, targeted tweaks for one app, each
 * turned on in THUMB when patching that app. Listed in options.json "fixes".
 */
final class GameFixes {
    private static final String TAG = "thumb";
    private static final Handler main = new Handler(Looper.getMainLooper());

    private GameFixes() {}

    static void start(Application app, JSONArray fixes) {
        if (fixes == null) return;
        for (int i = 0; i < fixes.length(); i++) {
            String id = fixes.optString(i);
            if ("worms3-audio-latency".equals(id)) wormsAudioLatency(app);
            else Log.w(TAG, "fixes: unknown fix " + id);
        }
    }

    // ---- Worms 3: lower audio delay ----
    // Worms refills its AudioTrack one "period" at a time: each time Android
    // reports a period played, it mixes and writes another. It sets the period
    // to a quarter of its buffer *in bytes*, which Android counts in frames, so
    // on modern phones each period is ~0.7 s (22 kHz mono) and over a second of
    // sound is always queued. The fix makes the period ~80 ms (Worms' own field
    // and the track's notification period) and the buffer two periods. The
    // track is recreated on pause/resume, so keep checking.

    private static WeakReference<AudioTrack> fixedTrack = new WeakReference<>(null);

    private static void wormsAudioLatency(final Application app) {
        main.postDelayed(new Runnable() {
            @Override public void run() {
                try {
                    Object thread = wormsAudioThread(app.getClassLoader());
                    Object track = thread == null ? null : field(thread.getClass(), thread, "m_audioTrack");
                    if (track instanceof AudioTrack && track != fixedTrack.get()) {
                        shrink(thread, (AudioTrack) track);
                        fixedTrack = new WeakReference<>((AudioTrack) track);
                    }
                } catch (Throwable t) {
                    Log.w(TAG, "fixes: worms3-audio-latency: " + t);
                }
                main.postDelayed(this, 2000);
            }
        }, 2000);
    }

    // Main.m_GLView (static) -> m_Renderer -> m_AudioMixer -> m_audioThread
    private static Object wormsAudioThread(ClassLoader cl) throws Exception {
        Object o = field(Class.forName("com.worms3.app.Main", false, cl), null, "m_GLView");
        for (String name : new String[] {"m_Renderer", "m_AudioMixer", "m_audioThread"}) {
            if (o == null) return null;
            o = field(o.getClass(), o, name);
        }
        return o;
    }

    private static void shrink(Object thread, AudioTrack track) throws Exception {
        int rate = track.getSampleRate();
        int before = track.getBufferSizeInFrames();
        if (rate <= 0 || before <= 0) return;
        int period = Math.max(256, rate * 80 / 1000);  // ~80 ms
        // Worms writes access$000() * 2 bytes per notification (16-bit), i.e. this many frames.
        setInt(thread, "m_nNotificationPeriod", period);
        track.setPositionNotificationPeriod(period);
        int after = track.setBufferSizeInFrames(Math.min(before, period * 2));
        Log.i(TAG, "fixes: worms3-audio-latency: period " + ms(period, rate) + " ms, buffer " + ms(before, rate)
            + " ms -> " + ms(after, rate) + " ms");
    }

    private static void setInt(Object target, String name, int value) throws Exception {
        for (Class<?> k = target.getClass(); k != null; k = k.getSuperclass()) {
            try {
                Field f = k.getDeclaredField(name);
                f.setAccessible(true);
                f.setInt(target, value);
                return;
            } catch (NoSuchFieldException e) {
                // look in the superclass
            }
        }
        throw new NoSuchFieldException(name);
    }

    private static Object field(Class<?> c, Object target, String name) throws Exception {
        for (Class<?> k = c; k != null; k = k.getSuperclass()) {
            try {
                Field f = k.getDeclaredField(name);
                f.setAccessible(true);
                return f.get(target);
            } catch (NoSuchFieldException e) {
                // look in the superclass
            }
        }
        return null;
    }

    private static long ms(int frames, int rate) { return rate > 0 ? frames * 1000L / rate : -1; }
}
