package dev.thumb.overlay;

import android.app.Application;
import android.media.AudioFormat;
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
    // Worms sizes its AudioTrack as 4x the minimum buffer (on Android 4.3+),
    // which on modern phones queues ~340 ms of sound. Shrink the playable part
    // to two of the chunks it writes (Android 7+ allows that on a live track).
    // The track is recreated on pause/resume, so keep checking.

    private static WeakReference<AudioTrack> fixedTrack = new WeakReference<>(null);

    private static void wormsAudioLatency(final Application app) {
        main.postDelayed(new Runnable() {
            @Override public void run() {
                try {
                    AudioTrack track = wormsTrack(app.getClassLoader());
                    if (track != null && track != fixedTrack.get()) {
                        shrink(track);
                        fixedTrack = new WeakReference<>(track);
                    }
                } catch (Throwable t) {
                    Log.w(TAG, "fixes: worms3-audio-latency: " + t);
                }
                main.postDelayed(this, 2000);
            }
        }, 2000);
    }

    // Main.m_GLView (static) -> m_Renderer -> m_AudioMixer -> m_audioThread -> m_audioTrack
    private static AudioTrack wormsTrack(ClassLoader cl) throws Exception {
        Object o = field(Class.forName("com.worms3.app.Main", false, cl), null, "m_GLView");
        for (String name : new String[] {"m_Renderer", "m_AudioMixer", "m_audioThread", "m_audioTrack"}) {
            if (o == null) return null;
            o = field(o.getClass(), o, name);
        }
        return o instanceof AudioTrack ? (AudioTrack) o : null;
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

    private static void shrink(AudioTrack track) {
        int period = track.getPositionNotificationPeriod();  // frames Worms writes per step
        int before = track.getBufferSizeInFrames();
        if (period <= 0 || before <= 0) return;
        int target = Math.min(before, period * 2);
        int after = track.setBufferSizeInFrames(target);
        int rate = track.getSampleRate();
        Log.i(TAG, "fixes: worms3-audio-latency: buffer " + ms(before, rate) + " ms -> " + ms(after, rate) + " ms");
    }

    private static long ms(int frames, int rate) { return rate > 0 ? frames * 1000L / rate : -1; }
}
