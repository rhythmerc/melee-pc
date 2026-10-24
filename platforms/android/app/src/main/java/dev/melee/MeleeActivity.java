package dev.melee;

import org.libsdl.app.SDLActivity;

public class MeleeActivity extends SDLActivity {

    @Override
    protected String[] getLibraries() {
        /* libpng is linked statically since the aurora sync to 77326d45
         * (extern/aurora/extern/CMakeLists.txt, _USE_SHARED is off on
         * Android), so a clean build ships no libpng16.so and loading it
         * failed before the activity had a window. */
        return new String[] {
            "melee"
        };
    }

    @Override
    protected String getMainFunction() {
        return "SDL_main";
    }

    @Override
    public org.libsdl.app.SDLSurface createSDLSurface(android.content.Context context) {
        return new dev.encounter.aurora.AuroraSurface(context);
    }

    private TouchOverlayView mTouchOverlay;

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        // Nothing else asks Android for the app-specific external directory,
        // so /sdcard/Android/data/dev.melee.game/files never exists (or exists
        // owned by adb's shell user if created by hand) and melee-env.txt,
        // which pc_env_file_bootstrap reads from there, can never load.
        // Asking once makes the system create it owned by this app.
        getExternalFilesDir(null);
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries) {
            return;
        }
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        applyImmersiveMode();

        mTouchOverlay = new TouchOverlayView(this);
        if (mLayout != null) {
            mLayout.addView(mTouchOverlay, new android.view.ViewGroup.LayoutParams(
                android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                android.view.ViewGroup.LayoutParams.MATCH_PARENT
            ));
        }

        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.N) {
            android.os.PowerManager pm = (android.os.PowerManager) getSystemService(android.content.Context.POWER_SERVICE);
            if (pm != null && pm.isSustainedPerformanceModeSupported()) {
                getWindow().setSustainedPerformanceMode(true);
            }
        }
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.M) {
            android.view.WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.preferredRefreshRate = 60.0f;
            getWindow().setAttributes(lp);
        }
        /* Only pre-Android 13 can be granted READ/WRITE_EXTERNAL_STORAGE at
         * all; from API 33 they are not grantable and the request resolves
         * instantly, but the dialog still pauses and recreates the activity
         * while the graphics device is coming up, which killed the process at
         * startup on a Pixel 8. The disc is read from the app's own external
         * files dir, which needs no permission on any version. */
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.M
                && android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.TIRAMISU) {
            if (checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE)
                    != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[] {
                    android.Manifest.permission.READ_EXTERNAL_STORAGE,
                    android.Manifest.permission.WRITE_EXTERNAL_STORAGE
                }, 100);
            }
        }
    }

    /* Called from native (src/pc/android_compat.cpp) around pc_launcher_run:
     * the launcher needs every touch while it is on screen. */
    public void setLauncherActive(final boolean active) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (mTouchOverlay != null) {
                    mTouchOverlay.setLauncherActive(active);
                }
                if (active) {
                    requestXrPermissions();
                }
            }
        });
    }

    /* Runtime permissions the Quest build's game uses when granted, and does
     * without otherwise: spatial data (USE_SCENE, both names; the stage's
     * shadow on the real surface under it). Asked for here, in the launcher
     * panel, once the graphics device is up, never in the immersive game: a
     * grant is the package's, so it covers the game's process too. Only the
     * ones this build's manifest declares (none on phones), once a launch;
     * Android itself stops asking after repeated denials. The passthrough
     * cameras (HEADSET_CAMERA) are not asked for here: only when the player
     * picks Roomtone lighting, after the launcher says why
     * (requestHeadsetCamera). */
    private static final String[] XR_PERMISSIONS = {
        "horizonos.permission.USE_SCENE",
        "com.oculus.permission.USE_SCENE",
    };
    private static final int XR_PERMISSIONS_REQUEST = 0x4d58; /* clear of SDL's own request codes */
    private boolean mXrPermissionsAsked;

    private void requestXrPermissions() {
        if (mXrPermissionsAsked || getClass() != MeleeActivity.class
                || android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.M) {
            return;
        }
        mXrPermissionsAsked = true;
        java.util.List<String> declared;
        try {
            android.content.pm.PackageManager pm = getPackageManager();
            android.content.pm.PackageInfo info =
                android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.TIRAMISU
                    ? pm.getPackageInfo(getPackageName(), android.content.pm.PackageManager.PackageInfoFlags.of(
                          android.content.pm.PackageManager.GET_PERMISSIONS))
                    : pm.getPackageInfo(getPackageName(), android.content.pm.PackageManager.GET_PERMISSIONS);
            String[] requested = info.requestedPermissions;
            declared = requested != null ? java.util.Arrays.asList(requested) : new java.util.ArrayList<String>();
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            return;
        }
        java.util.List<String> ask = new java.util.ArrayList<>();
        for (String p : XR_PERMISSIONS) {
            if (!declared.contains(p)) {
                continue;
            }
            boolean granted = checkSelfPermission(p) == android.content.pm.PackageManager.PERMISSION_GRANTED;
            android.util.Log.i("melee", "permission " + p + (granted ? ": granted" : ": not granted, asking"));
            if (!granted) {
                ask.add(p);
            }
        }
        if (!ask.isEmpty()) {
            requestPermissions(ask.toArray(new String[0]), XR_PERMISSIONS_REQUEST);
        }
    }

    /* The passthrough cameras, for Roomtone lighting (the room's light,
     * RoomCapture). Called from native (src/pc/android_compat.cpp) by the
     * launcher's Lighting row, after its own explanation. */
    private static final String HEADSET_CAMERA = "horizonos.permission.HEADSET_CAMERA";
    private static final int CAMERA_REQUEST = 0x4d59;
    /* The last request's answer: -1 asking, 0 declined, 1 granted, 2 declined
     * and Android won't ask again (the system settings can still allow it). */
    private static volatile int sCameraResult = -1;

    public boolean hasHeadsetCamera() {
        return android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.M
            || checkSelfPermission(HEADSET_CAMERA) == android.content.pm.PackageManager.PERMISSION_GRANTED;
    }

    public void requestHeadsetCamera() {
        if (hasHeadsetCamera()) {
            sCameraResult = 1;
            return;
        }
        sCameraResult = -1;
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                android.util.Log.i("melee", "permission " + HEADSET_CAMERA + ": asking (Roomtone)");
                requestPermissions(new String[] {HEADSET_CAMERA}, CAMERA_REQUEST);
            }
        });
    }

    public int headsetCameraResult() {
        return sCameraResult;
    }

    /* Xelee's page in the system settings, where a permission Android won't
     * ask for again can still be allowed. */
    public void openAppSettings() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    android.content.Intent intent = new android.content.Intent(
                        android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        android.net.Uri.fromParts("package", getPackageName(), null));
                    intent.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
                    startActivity(intent);
                } catch (android.content.ActivityNotFoundException e) {
                    android.util.Log.w("melee", "no app settings page: " + e);
                }
            }
        });
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode == CAMERA_REQUEST) {
            final boolean granted = grantResults.length > 0
                && grantResults[0] == android.content.pm.PackageManager.PERMISSION_GRANTED;
            /* Declined with no rationale to show: Android won't ask again. */
            sCameraResult = granted ? 1 : (shouldShowRequestPermissionRationale(HEADSET_CAMERA) ? 0 : 2);
            android.util.Log.i("melee", "permission " + HEADSET_CAMERA + ": "
                + (granted ? "granted" : sCameraResult == 2 ? "denied, won't ask again" : "denied"));
            return;
        }
        if (requestCode != XR_PERMISSIONS_REQUEST) {
            super.onRequestPermissionsResult(requestCode, permissions, grantResults);
            return;
        }
        for (int i = 0; i < permissions.length && i < grantResults.length; i++) {
            android.util.Log.i("melee", "permission " + permissions[i] + ": "
                + (grantResults[i] == android.content.pm.PackageManager.PERMISSION_GRANTED ? "granted" : "denied"));
        }
    }

    /* Meta Quest hybrid builds (MELEE_XR=1): the launcher runs in this 2D
     * panel activity and the game in the immersive MeleeXrActivity, which has
     * its own process. Called from native (src/pc/android_compat.cpp) once the
     * launcher's Play button has picked a disc. */
    public void launchXrGame(final String disc) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                android.content.Intent intent =
                    new android.content.Intent(MeleeActivity.this, MeleeXrActivity.class);
                intent.setAction(android.content.Intent.ACTION_MAIN);
                intent.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
                intent.putExtra("disc", disc);
                startActivity(intent);
                finishAndRemoveTask();
            }
        });
    }

    /* The updater (src/pc/updater.cpp): a release's APK into the app's cache.
     * Blocking, on the updater's worker thread (never the UI thread); true
     * when the whole body arrived. GitHub's asset URL redirects to its CDN,
     * which HttpURLConnection follows (https to https). */
    public boolean downloadFile(final String url, final String path) {
        java.net.HttpURLConnection conn = null;
        java.io.File file = new java.io.File(path);
        try {
            conn = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
            conn.setConnectTimeout(15000);
            conn.setReadTimeout(30000);
            conn.setRequestProperty("User-Agent", "Xelee-Updater");
            conn.setInstanceFollowRedirects(true);
            if (conn.getResponseCode() != 200) {
                return false;
            }
            try (java.io.InputStream in = conn.getInputStream();
                 java.io.OutputStream out = new java.io.FileOutputStream(file)) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; ) {
                    out.write(buf, 0, n);
                }
            }
            return true;
        } catch (Exception e) {
            android.util.Log.e("melee", "update download failed", e);
            file.delete();
            return false;
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
    }

    private static final String INSTALL_STATUS = "dev.melee.game.INSTALL_STATUS";
    private android.content.BroadcastReceiver mInstallStatus;

    /* Hand a downloaded APK to the system installer through a PackageInstaller
     * session (no FileProvider, so no androidx). The first time, Android asks
     * the player to allow installs from this app; after that it shows its own
     * "update this app?" confirmation, and the app restarts updated. */
    public void installApk(final String path) {
        runOnUiThread(() -> {
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O
                    && !getPackageManager().canRequestPackageInstalls()) {
                try {
                    startActivity(new android.content.Intent(
                            android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                            android.net.Uri.parse("package:" + getPackageName())));
                } catch (Exception e) {
                    android.util.Log.e("melee", "no unknown-sources settings page", e);
                }
                return;
            }
            if (mInstallStatus == null) {
                mInstallStatus = new android.content.BroadcastReceiver() {
                    @Override
                    public void onReceive(android.content.Context context, android.content.Intent intent) {
                        int status = intent.getIntExtra(android.content.pm.PackageInstaller.EXTRA_STATUS, -1);
                        if (status == android.content.pm.PackageInstaller.STATUS_PENDING_USER_ACTION) {
                            android.content.Intent confirm = intent.getParcelableExtra(android.content.Intent.EXTRA_INTENT);
                            if (confirm != null) {
                                confirm.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
                                startActivity(confirm);
                            }
                        } else if (status != android.content.pm.PackageInstaller.STATUS_SUCCESS) {
                            android.util.Log.e("melee", "update install: status " + status + " "
                                    + intent.getStringExtra(android.content.pm.PackageInstaller.EXTRA_STATUS_MESSAGE));
                        }
                    }
                };
                android.content.IntentFilter filter = new android.content.IntentFilter(INSTALL_STATUS);
                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.TIRAMISU) {
                    registerReceiver(mInstallStatus, filter, android.content.Context.RECEIVER_NOT_EXPORTED);
                } else {
                    registerReceiver(mInstallStatus, filter);
                }
            }
            new Thread(() -> {
                try {
                    android.content.pm.PackageInstaller installer = getPackageManager().getPackageInstaller();
                    android.content.pm.PackageInstaller.SessionParams params =
                            new android.content.pm.PackageInstaller.SessionParams(
                                    android.content.pm.PackageInstaller.SessionParams.MODE_FULL_INSTALL);
                    params.setAppPackageName(getPackageName());
                    int id = installer.createSession(params);
                    java.io.File file = new java.io.File(path);
                    try (android.content.pm.PackageInstaller.Session session = installer.openSession(id)) {
                        try (java.io.InputStream in = new java.io.FileInputStream(file);
                             java.io.OutputStream out = session.openWrite("update.apk", 0, file.length())) {
                            byte[] buf = new byte[1 << 16];
                            for (int n; (n = in.read(buf)) > 0; ) {
                                out.write(buf, 0, n);
                            }
                            session.fsync(out);
                        }
                        android.content.Intent status = new android.content.Intent(INSTALL_STATUS).setPackage(getPackageName());
                        int flags = android.app.PendingIntent.FLAG_UPDATE_CURRENT
                                | (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.S
                                        ? android.app.PendingIntent.FLAG_MUTABLE : 0);
                        session.commit(android.app.PendingIntent.getBroadcast(this, id, status, flags).getIntentSender());
                    }
                } catch (Exception e) {
                    android.util.Log.e("melee", "update install failed", e);
                }
            }, "update-install").start();
        });
    }

    /* The reverse, for an immersive activity that has nothing to show (started
     * without a disc): reopen the launcher panel in the Home environment,
     * the way Meta's hybrid-app guide does it. */
    public void launchPanel() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                android.content.Context context = getApplicationContext();
                android.content.Intent panel =
                    new android.content.Intent(context, MeleeActivity.class);
                panel.setAction(android.content.Intent.ACTION_MAIN);
                panel.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
                android.app.PendingIntent pending = android.app.PendingIntent.getActivity(context, 0,
                    panel, android.app.PendingIntent.FLAG_UPDATE_CURRENT
                        | android.app.PendingIntent.FLAG_IMMUTABLE);
                android.content.Intent home = new android.content.Intent(android.content.Intent.ACTION_MAIN);
                home.addCategory(android.content.Intent.CATEGORY_HOME);
                home.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
                home.putExtra("extra_launch_in_home_pending_intent", pending);
                startActivity(home);
                finishAndRemoveTask();
            }
        });
    }

    public static native void nativeDisconnect();

    @Override
    protected void onStop() {
        if (isFinishing()) {
            try {
                nativeDisconnect();
            } catch (Throwable ignored) {}
        }
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        try {
            nativeDisconnect();
        } catch (Throwable ignored) {}
        super.onDestroy();
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyImmersiveMode();
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.M) {
            android.view.WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.preferredRefreshRate = 60.0f;
            getWindow().setAttributes(lp);
        }
        if (mTouchOverlay != null) {
            mTouchOverlay.updateControllerState();
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyImmersiveMode();
        }
    }

    private void applyImmersiveMode() {
        if (mBrokenLibraries) {
            return;
        }
        android.view.Window window = getWindow();
        if (window == null) {
            return;
        }
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            android.view.WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(android.view.WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(
                    android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.P) {
                android.view.WindowManager.LayoutParams lp = window.getAttributes();
                lp.layoutInDisplayCutoutMode =
                    android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
                window.setAttributes(lp);
            }
        } else {
            int flags = android.view.View.SYSTEM_UI_FLAG_FULLSCREEN
                | android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
            window.getDecorView().setSystemUiVisibility(flags);
        }
    }

    @Override
    protected String[] getArguments() {
        android.content.Intent intent = getIntent();
        if (intent != null) {
            String[] args = intent.getStringArrayExtra("args");
            if (args != null && args.length > 0) {
                return args;
            }
            String disc = intent.getStringExtra("disc");
            if (disc != null && !disc.isEmpty()) {
                // For tools/quest_perf.py: a release build's launcher.cfg is
                // out of adb's reach (no run-as), and the disc is a document
                // URI only this app can open, so the last one played is kept
                // where adb can read it.
                try (java.io.FileWriter w = new java.io.FileWriter(
                         new java.io.File(getExternalFilesDir(null), "last-disc.txt"))) {
                    w.write(disc + "\n");
                } catch (Exception ignored) {
                }
                return new String[] { "--dvd", disc };
            }
            android.net.Uri data = intent.getData();
            if (data != null) {
                try {
                    getContentResolver().takePersistableUriPermission(
                        data, android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (Exception ignored) {
                }
                return new String[] { "--dvd", data.toString() };
            }
        }
        return new String[0];
    }

    @Override
    public boolean dispatchKeyEvent(android.view.KeyEvent event) {
        /* The system's own keys go to Android, as SDLActivity's dispatch does:
         * consumed here, the headset's volume buttons did nothing in game. */
        switch (event.getKeyCode()) {
        case android.view.KeyEvent.KEYCODE_VOLUME_UP:
        case android.view.KeyEvent.KEYCODE_VOLUME_DOWN:
        case android.view.KeyEvent.KEYCODE_VOLUME_MUTE:
        case android.view.KeyEvent.KEYCODE_CAMERA:
        case android.view.KeyEvent.KEYCODE_ZOOM_IN:
        case android.view.KeyEvent.KEYCODE_ZOOM_OUT:
            return super.dispatchKeyEvent(event);
        default:
            break;
        }
        if (mSurface != null) {
            handleKeyEvent(mSurface, event.getKeyCode(), event, null);
            return true;
        }
        return super.dispatchKeyEvent(event);
    }
}
