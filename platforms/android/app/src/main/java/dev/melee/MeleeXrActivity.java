package dev.melee;

/* The immersive half of the Meta Quest hybrid build (MELEE_XR=1). It is only
 * declared in the XR manifest overlay (platforms/android/app/src/xr), where
 * it runs in its own ":xr" process: SDL keeps process-wide state, so the
 * launcher panel and the game each get a fresh process. MeleeActivity
 * launches it with the chosen disc as the "disc" extra, which skips the
 * launcher (MeleeActivity.getArguments). */
public class MeleeXrActivity extends MeleeActivity {
    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        /* Before SDL loads the native library and starts main. AURORA_XR turns
         * on OpenXR presentation (extern/aurora/lib/xr); MELEE_XR_ACTIVITY
         * tells main.c which half of the hybrid app this process is. */
        try {
            android.system.Os.setenv("AURORA_XR", "1", true);
            android.system.Os.setenv("MELEE_XR_ACTIVITY", "1", true);
        } catch (android.system.ErrnoException e) {
            android.util.Log.e("melee", "setenv failed", e);
        }
        super.onCreate(savedInstanceState);
    }
}
