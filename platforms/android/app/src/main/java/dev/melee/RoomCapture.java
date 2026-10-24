package dev.melee;

import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;
import android.util.Log;

/* The room's light, captured for XR room lighting (extern/aurora/lib/xr,
 * "room capture"; the dev tools' Light page, source ROOM). Quest builds
 * only: MeleeXrActivity runs one in the immersive game's process.
 *
 * Four times a second it asks the game whether a capture is running
 * (aurora_xr_room_capture_wanted: the 20 s after ROOM is chosen, or ambient
 * while it's the source, in the menus and while the stage is placed, when
 * only a frame each AMBIENT_MS is used). While one is, with the passthrough camera
 * permission (horizonos.permission.HEADSET_CAMERA, asked for by the launcher
 * when Roomtone lighting is picked), it streams one passthrough camera (the
 * left one, by Meta's vendor tags) at 1280x1280, its square size (the whole sensor, scaled, so its
 * calibration applies as it is), at its lowest frame rate. Each frame
 * becomes a 32x32 grid of mean linear RGB (BT.601 full range, decoded from
 * sRGB) and the share of each cell that clipped (luma 250 or more: a lamp
 * brighter than the frame's exposure can show), with its exposure and
 * sensitivity, handed to the game, which turns
 * it into the room's light by where the head looked. The camera's lens pose
 * and intrinsics go once per capture. The camera shares passthrough's auto
 * exposure; its frames never leave this thread, only the grids do.
 *
 * AURORA_XR_ROOM_CAPTURE_LOG=1 logs the camera picked, and each second's
 * frames (tag MeleeCapture). */
final class RoomCapture {
    private static final String TAG = "MeleeCapture";
    private static final long POLL_MS = 250;
    private static final int GRID = 32;
    private static final int STEP = 5; /* every 5th pixel of a cell, each way */
    private static final long AMBIENT_MS = 200; /* xr.cpp's kCapAmbientMs */

    private static native int nativeWanted();

    private static native void nativeCamera(float[] rotation, float[] translation, float[] intrinsics, int sensorWidth,
            int sensorHeight, int width, int height);

    private static native void nativeFrame(float[] rgb, int cols, int rows, float exposureMs, float iso);

    private final android.content.Context mContext;
    private android.os.HandlerThread mThread;
    private android.os.Handler mHandler;
    private boolean mLog;
    private boolean mSetUp;
    private String mCameraId; /* null: no camera to use */
    private android.util.Size mSize;
    private android.util.Range<Integer> mFps;
    private float[] mRotation, mTranslation, mIntrinsics;
    private int mSensorW, mSensorH;
    /* A capture in progress (the handler's thread only). */
    private CameraDevice mDevice;
    private CameraCaptureSession mSession;
    private android.media.ImageReader mReader;
    private volatile CaptureResult mResult;
    private final float[] mGrid = new float[GRID * GRID * 4];
    private final float[] mLinear = new float[256];
    private int mFrames;
    private volatile int mWanted; /* nativeWanted's last: 1 a capture, 2 ambient (a frame each AMBIENT_MS) */
    private long mReducedAt;
    private int mFailures; /* this capture's; it gives up at 3 */
    private long mLoggedAt;

    private final Runnable mPoll = this::poll;

    RoomCapture(android.content.Context context) {
        mContext = context.getApplicationContext();
        for (int i = 0; i < 256; i++) {
            float c = i / 255f;
            mLinear[i] = c <= 0.04045f ? c / 12.92f : (float) Math.pow((c + 0.055f) / 1.055f, 2.4f);
        }
    }

    void start() {
        if (mThread != null || android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.P) {
            return;
        }
        mThread = new android.os.HandlerThread("melee-room-capture");
        mThread.start();
        mHandler = new android.os.Handler(mThread.getLooper());
        mHandler.postDelayed(mPoll, POLL_MS);
    }

    void stop() {
        if (mThread == null) {
            return;
        }
        final android.os.HandlerThread thread = mThread;
        mHandler.removeCallbacksAndMessages(null);
        mHandler.post(() -> {
            closeCamera();
            thread.quitSafely();
        });
        mThread = null;
    }

    private void poll() {
        int wanted;
        try {
            wanted = nativeWanted();
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "no native room capture: " + e);
            return;
        }
        mWanted = wanted;
        if (wanted > 0 && mDevice == null && mReader == null) {
            if (!mSetUp) {
                mSetUp = true;
                String v = android.system.Os.getenv("AURORA_XR_ROOM_CAPTURE_LOG");
                mLog = v != null && !v.isEmpty() && v.charAt(0) == '1';
                pickCamera();
            }
            if (mCameraId != null && mFailures < 3) {
                nativeCamera(mRotation, mTranslation, mIntrinsics, mSensorW, mSensorH, mSize.getWidth(),
                    mSize.getHeight());
                open();
            }
        } else if (wanted <= 0) {
            mFailures = 0;
            if (mDevice != null || mReader != null) {
                closeCamera();
                Log.i(TAG, "capture done: " + mFrames + " frames");
            }
        }
        mHandler.postDelayed(mPoll, POLL_MS);
    }

    private boolean granted(String permission) {
        return mContext.checkSelfPermission(permission) == android.content.pm.PackageManager.PERMISSION_GRANTED;
    }

    /* A characteristic by name: Meta's vendor tags aren't public keys. */
    private static Object characteristic(CameraCharacteristics ch, String name) {
        for (CameraCharacteristics.Key<?> key : ch.getKeys()) {
            if (key.getName().equals(name)) {
                return ch.get(key);
            }
        }
        return null;
    }

    /* A vendor tag's number: Meta's come as byte[1], not as a Number. -1: none. */
    private static int tagInt(Object v) {
        if (v instanceof Number) {
            return ((Number) v).intValue();
        }
        if (v instanceof byte[] && ((byte[]) v).length > 0) {
            return ((byte[]) v)[0];
        }
        return -1;
    }

    /* The left passthrough camera (camera_source 0, position 0), with its
     * calibration and a square size. Never another camera: those need
     * android.permission.CAMERA. */
    private void pickCamera() {
        if (!granted("horizonos.permission.HEADSET_CAMERA")) {
            Log.i(TAG, "no camera permission (HEADSET_CAMERA): no room capture");
            return;
        }
        CameraManager cm = (CameraManager) mContext.getSystemService(android.content.Context.CAMERA_SERVICE);
        if (cm == null) {
            return;
        }
        try {
            for (String id : cm.getCameraIdList()) {
                CameraCharacteristics ch = cm.getCameraCharacteristics(id);
                if (tagInt(characteristic(ch, "com.meta.extra_metadata.camera_source")) != 0
                        || tagInt(characteristic(ch, "com.meta.extra_metadata.position")) != 0) {
                    continue; /* not the left passthrough camera */
                }
                float[] rotation = ch.get(CameraCharacteristics.LENS_POSE_ROTATION);
                float[] translation = ch.get(CameraCharacteristics.LENS_POSE_TRANSLATION);
                float[] intrinsics = ch.get(CameraCharacteristics.LENS_INTRINSIC_CALIBRATION);
                android.graphics.Rect array = ch.get(CameraCharacteristics.SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE);
                if (rotation == null || translation == null || intrinsics == null || array == null) {
                    Log.w(TAG, "camera " + id + " has no calibration");
                    continue;
                }
                android.util.Size square = null;
                for (android.util.Size s : ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                        .getOutputSizes(android.graphics.ImageFormat.YUV_420_888)) {
                    if (s.getWidth() == s.getHeight() && s.getWidth() >= GRID * STEP
                            && (square == null || s.getWidth() < square.getWidth())) {
                        square = s;
                    }
                }
                if (square == null) {
                    Log.w(TAG, "camera " + id + " has no square size");
                    continue;
                }
                android.util.Range<Integer>[] fps = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
                mFps = null;
                if (fps != null) {
                    for (android.util.Range<Integer> r : fps) {
                        if (mFps == null || r.getUpper() < mFps.getUpper()) {
                            mFps = r;
                        }
                    }
                }
                mCameraId = id;
                mSize = square;
                mRotation = rotation;
                mTranslation = translation;
                mIntrinsics = intrinsics;
                mSensorW = array.width();
                mSensorH = array.height();
                Log.i(TAG, "room capture from camera " + id + " at " + square + ", " + mFps + " fps; sensor "
                    + mSensorW + "x" + mSensorH + ", intrinsics " + java.util.Arrays.toString(intrinsics)
                    + ", pose " + java.util.Arrays.toString(rotation) + " " + java.util.Arrays.toString(translation));
                return;
            }
            Log.w(TAG, "no left passthrough camera: no room capture");
        } catch (Exception e) {
            Log.w(TAG, "cameras unavailable: " + e);
            mCameraId = null;
        }
    }

    private void open() {
        mFrames = 0;
        mResult = null;
        try {
            CameraManager cm = (CameraManager) mContext.getSystemService(android.content.Context.CAMERA_SERVICE);
            mReader = android.media.ImageReader.newInstance(mSize.getWidth(), mSize.getHeight(),
                android.graphics.ImageFormat.YUV_420_888, 2);
            mReader.setOnImageAvailableListener(this::onImage, mHandler);
            cm.openCamera(mCameraId, new CameraDevice.StateCallback() {
                @Override
                public void onOpened(CameraDevice device) {
                    mDevice = device;
                    configure();
                }

                @Override
                public void onDisconnected(CameraDevice device) {
                    mDevice = device;
                    failed("disconnected");
                }

                @Override
                public void onError(CameraDevice device, int error) {
                    mDevice = device;
                    failed("error " + error);
                }
            }, mHandler);
        } catch (Exception e) {
            failed(e.toString());
        }
    }

    private void configure() {
        final android.view.Surface surface = mReader.getSurface();
        try {
            java.util.List<android.hardware.camera2.params.OutputConfiguration> outputs =
                java.util.Collections.singletonList(new android.hardware.camera2.params.OutputConfiguration(surface));
            mDevice.createCaptureSession(new android.hardware.camera2.params.SessionConfiguration(
                android.hardware.camera2.params.SessionConfiguration.SESSION_REGULAR, outputs, mHandler::post,
                new CameraCaptureSession.StateCallback() {
                    @Override
                    public void onConfigured(CameraCaptureSession session) {
                        mSession = session;
                        try {
                            CaptureRequest.Builder b = mDevice.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                            b.addTarget(surface);
                            if (mFps != null) {
                                b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, mFps);
                            }
                            session.setRepeatingRequest(b.build(), new CameraCaptureSession.CaptureCallback() {
                                @Override
                                public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest request,
                                        TotalCaptureResult result) {
                                    mResult = result;
                                }
                            }, mHandler);
                        } catch (Exception e) {
                            failed(e.toString());
                        }
                    }

                    @Override
                    public void onConfigureFailed(CameraCaptureSession session) {
                        failed("session not configured");
                    }
                }));
        } catch (Exception e) {
            failed(e.toString());
        }
    }

    private void onImage(android.media.ImageReader reader) {
        android.media.Image image = reader.acquireLatestImage();
        if (image == null) {
            return;
        }
        long at = android.os.SystemClock.elapsedRealtime();
        if (mWanted == 2 && at - mReducedAt < AMBIENT_MS) {
            image.close();
            return;
        }
        mReducedAt = at;
        try {
            reduce(image);
            if (mLog && (mFrames == 30 || mFrames == 150 || mFrames == 280)) {
                dump(image, mFrames);
            }
        } finally {
            image.close();
        }
        CaptureResult result = mResult;
        Long exposure = result != null ? result.get(CaptureResult.SENSOR_EXPOSURE_TIME) : null;
        Integer iso = result != null ? result.get(CaptureResult.SENSOR_SENSITIVITY) : null;
        float exposureMs = exposure != null ? exposure / 1e6f : -1f;
        nativeFrame(mGrid, GRID, GRID, exposureMs, iso != null ? iso : -1f);
        mFrames++;
        long now = android.os.SystemClock.elapsedRealtime();
        if (mLog && now - mLoggedAt >= 1000) {
            mLoggedAt = now;
            Log.i(TAG, mFrames + " frames, exposure " + exposureMs + " ms, ISO " + iso);
        }
    }

    /* The frame as GRID x GRID cells of mean linear RGB and the share
     * clipped, each from every STEP-th pixel of it. Full-range YCbCr
     * (BT.601, as camera YUV is). */
    private void reduce(android.media.Image image) {
        android.media.Image.Plane[] p = image.getPlanes();
        java.nio.ByteBuffer y = p[0].getBuffer(), u = p[1].getBuffer(), v = p[2].getBuffer();
        int yRow = p[0].getRowStride(), yPix = p[0].getPixelStride();
        int uRow = p[1].getRowStride(), uPix = p[1].getPixelStride();
        int vRow = p[2].getRowStride(), vPix = p[2].getPixelStride();
        int cw = image.getWidth() / GRID, ch = image.getHeight() / GRID;
        for (int gy = 0; gy < GRID; gy++) {
            for (int gx = 0; gx < GRID; gx++) {
                float sr = 0f, sg = 0f, sb = 0f;
                int n = 0, clipped = 0;
                for (int row = gy * ch + STEP / 2; row < (gy + 1) * ch; row += STEP) {
                    for (int col = gx * cw + STEP / 2; col < (gx + 1) * cw; col += STEP) {
                        float l = y.get(row * yRow + col * yPix) & 0xff;
                        if (l >= 250f) {
                            clipped++;
                        }
                        float cb = (u.get((row / 2) * uRow + (col / 2) * uPix) & 0xff) - 128f;
                        float cr = (v.get((row / 2) * vRow + (col / 2) * vPix) & 0xff) - 128f;
                        sr += mLinear[clamp(l + 1.402f * cr)];
                        sg += mLinear[clamp(l - 0.344136f * cb - 0.714136f * cr)];
                        sb += mLinear[clamp(l + 1.772f * cb)];
                        n++;
                    }
                }
                int i = (gy * GRID + gx) * 4;
                mGrid[i] = n > 0 ? sr / n : 0f;
                mGrid[i + 1] = n > 0 ? sg / n : 0f;
                mGrid[i + 2] = n > 0 ? sb / n : 0f;
                mGrid[i + 3] = n > 0 ? (float) clipped / n : 0f;
            }
        }
    }

    /* With the capture log: the frame's luma, halved, as a PGM in the
     * app's files (room-frame-N.pgm), to check what the cells saw. */
    private void dump(android.media.Image image, int frame) {
        android.media.Image.Plane p = image.getPlanes()[0];
        java.nio.ByteBuffer y = p.getBuffer();
        int row = p.getRowStride(), pix = p.getPixelStride();
        int w = image.getWidth() / 2, h = image.getHeight() / 2;
        byte[] out = new byte[w * h];
        for (int r = 0; r < h; r++) {
            for (int c = 0; c < w; c++) {
                out[r * w + c] = y.get(2 * r * row + 2 * c * pix);
            }
        }
        java.io.File f = new java.io.File(mContext.getExternalFilesDir(null), "room-frame-" + frame + ".pgm");
        try (java.io.FileOutputStream o = new java.io.FileOutputStream(f)) {
            o.write(("P5\n" + w + " " + h + "\n255\n").getBytes());
            o.write(out);
        } catch (java.io.IOException e) {
            Log.w(TAG, "frame dump: " + e);
        }
    }

    private static int clamp(float c) {
        int i = Math.round(c);
        return i < 0 ? 0 : i > 255 ? 255 : i;
    }

    private void failed(String why) {
        closeCamera();
        mFailures++;
        Log.w(TAG, "room capture failed: " + why);
    }

    private void closeCamera() {
        if (mSession != null) {
            mSession.close();
            mSession = null;
        }
        if (mDevice != null) {
            mDevice.close();
            mDevice = null;
        }
        if (mReader != null) {
            mReader.close();
            mReader = null;
        }
    }
}
