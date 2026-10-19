package com.soundshed.guitar;

import android.content.Context;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.lang.reflect.Method;

import dalvik.system.PathClassLoader;

/**
 * A CPU clock floor from Samsung's Galaxy Performance SDK, held while the app is in front.
 *
 * <p>The audio callback is a short, steady load: one block every two milliseconds at about a
 * fifth of the budget. Samsung's energy_step governor reads that as an idle cluster and drops
 * the big cores to their lowest clock, where the same DSP takes two to three times as long and
 * the worst block of each UI frame comes close to the deadline. Phones with ADPF can be told
 * the budget; a Galaxy S21 has neither ADPF nor sustained performance mode, and the governor
 * ignores per-thread uclamp. What it does honour is a minimum-frequency request, and Samsung's
 * performance SDK lets any foreground app make one. Its classes ship in the phone's own
 * {@code /system/framework/perfsdk.jar}, so they are loaded from there rather than bundled; on
 * other phones the jar is missing and this does nothing.
 *
 * <p>The SDK refuses a request longer than one second, so the floor is renewed twice a second
 * while the activity is resumed and dropped on pause; it never outlives the process by more
 * than a second. {@code debug.soundshed.audio.clock} picks it without a rebuild:
 *
 * <pre>
 *     adb shell setprop debug.soundshed.audio.clock auto       (default: level 1)
 *     adb shell setprop debug.soundshed.audio.clock sperf:2    (levels 0-4)
 *     adb shell setprop debug.soundshed.audio.clock off        (no floor)
 *     adb shell am force-stop com.soundshed.guitar             (then relaunch)
 * </pre>
 *
 * <p>On a Galaxy S21 the levels are 1248, 1456, 1768, 1768 and 1768 MHz on the A78 and X1
 * cores, with the A55s held at 1170 MHz.
 */
final class ClockFloor
{
    private static final String TAG = "SoundshedGuitar";
    private static final String PROPERTY = "debug.soundshed.audio.clock";
    private static final String SDK_JAR = "/system/framework/perfsdk.jar";
    private static final int TYPE_CPU_MIN = 0; // CustomParams.TYPE_CPU_MIN
    private static final int DEFAULT_LEVEL = 1;
    private static final int TIMEOUT_MS = 1000; // the longest request the SDK accepts
    private static final int RENEW_MS = 500;
    private static final int OFF = -2;

    private final Context context;
    private final HandlerThread thread = new HandlerThread ("ClockFloor");
    private final Handler handler;

    // Handler thread only.
    private int level = -1; // -1 until the property is read
    private Class<?> sdk;   // SPerf, loaded once: its native library binds to a single class loader
    private Object manager; // PerformanceManager
    private Class<?> paramsClass;
    private Method add, start, stop;
    private boolean holding;
    private int bindAttempts;

    ClockFloor (Context context)
    {
        this.context = context.getApplicationContext();
        thread.start();
        handler = new Handler (thread.getLooper());
    }

    /** From onResume: starts the floor, unless the property turns it off. */
    void hold()
    {
        handler.post (() -> {
            if (level == -1)
                level = readLevel();

            holding = true;
            handler.removeCallbacks (begin);
            handler.removeCallbacks (renew);
            handler.post (begin);
        });
    }

    /** From onPause: drops the floor at once. */
    void release()
    {
        handler.post (() -> {
            holding = false;
            handler.removeCallbacks (begin);
            handler.removeCallbacks (renew);

            if (manager != null)
            {
                try { stop.invoke (manager); }
                catch (Throwable t) { Log.w (TAG, "Clock floor: stop failed", t); }
            }
        });
    }

    /** From onDestroy. */
    void close()
    {
        release();
        thread.quitSafely();
    }

    // Binds if need be, then starts renewing. A quick restart can catch perfsdkserver still
    // tearing down the previous process's session, so initialisation is tried a few times.
    private final Runnable begin = new Runnable()
    {
        @Override public void run()
        {
            if (! holding || level < 0)
                return;

            if (manager == null && ! bind())
            {
                if (level >= 0 && ++bindAttempts < 5)
                    handler.postDelayed (this, 2000);

                return;
            }

            handler.post (renew);
        }
    };

    private final Runnable renew = new Runnable()
    {
        @Override public void run()
        {
            if (holding && request())
                handler.postDelayed (this, RENEW_MS);
        }
    };

    private static int readLevel()
    {
        String value = "";

        // android.os.SystemProperties is not public API; getprop is.
        try
        {
            final Process p = Runtime.getRuntime().exec (new String[] { "getprop", PROPERTY });

            try (BufferedReader r = new BufferedReader (new InputStreamReader (p.getInputStream())))
            {
                final String line = r.readLine();
                value = line != null ? line.trim() : "";
            }
        }
        catch (Exception ignored) {}

        if (value.equals ("off"))
            return OFF;

        if (value.startsWith ("sperf:"))
        {
            try { return Math.max (0, Math.min (4, Integer.parseInt (value.substring (6)))); }
            catch (NumberFormatException ignored) {}
        }

        return DEFAULT_LEVEL;
    }

    private boolean bind()
    {
        if (! new File (SDK_JAR).exists())
        {
            level = OFF;
            return false;
        }

        try
        {
            if (sdk == null)
                sdk = Class.forName ("com.samsung.sdk.sperf.SPerf", true,
                                     new PathClassLoader (SDK_JAR, context.getClassLoader()));

            final boolean initialised = (Boolean) sdk.getMethod ("initialize", Context.class).invoke (null, context);
            final ClassLoader loader = sdk.getClassLoader();
            final Class<?> pm = Class.forName ("com.samsung.sdk.sperf.PerformanceManager", true, loader);
            paramsClass = Class.forName ("com.samsung.sdk.sperf.CustomParams", true, loader);
            add = paramsClass.getMethod ("add", int.class, int.class, int.class);
            start = pm.getMethod ("start", paramsClass);
            stop = pm.getMethod ("stop");
            manager = initialised ? pm.getMethod ("getInstance").invoke (null) : null;

            Log.i (TAG, manager != null ? "Clock floor: Samsung performance SDK, level " + level
                                        : "Clock floor: Samsung performance SDK did not initialise");
            return manager != null;
        }
        catch (Throwable t)
        {
            Log.i (TAG, "Clock floor: Samsung performance SDK unavailable: " + t);
            level = OFF;
            return false;
        }
    }

    private boolean request()
    {
        try
        {
            final Object params = paramsClass.getConstructor().newInstance();

            if ((Integer) add.invoke (params, TYPE_CPU_MIN, level, TIMEOUT_MS) == 0
                && (Integer) start.invoke (manager, params) == 0)
                return true;

            Log.w (TAG, "Clock floor: request refused, giving up");
        }
        catch (Throwable t)
        {
            Log.w (TAG, "Clock floor: request failed", t);
        }

        level = OFF;
        return false;
    }
}
