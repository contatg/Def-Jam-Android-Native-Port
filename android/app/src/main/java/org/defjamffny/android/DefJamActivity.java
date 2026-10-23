package org.defjamffny.android;

import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.content.pm.ActivityInfo;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

import org.libsdl.app.SDLActivity;

public final class DefJamActivity extends SDLActivity {
    private static final String ROOT_NAME = "DefJamFFNY";
    private final Handler hudHandler = new Handler(Looper.getMainLooper());
    private TextView fpsView;
    private VirtualControlsView controls;
    private ControlsEditorPanel editor;
    private android.widget.LinearLayout quickBar;
    private boolean startHeld;
    private android.widget.Button startButton;
    private XboxMenuControlsView menuControls;
    private PortTouchView portTouch;
    private int appliedInputMode = -1;
    private static native int nativeGetInputMode();
    private static native float nativeGetFps();
    private static native float nativeGetGeneratedFps();

    private final Runnable inputUpdate = new Runnable() {
        @Override public void run() {
            if (!isFinishing()) {
                updateInputMode();
                hudHandler.postDelayed(this, 100);
            }
        }
    };

    private final Runnable fpsUpdate = new Runnable() {
        @Override public void run() {
            if (fpsView != null) {
                try {
                    float fps = nativeGetFps();
                    String rate = fps > .1f ? String.format(Locale.US, "%.1f FPS", fps) : "-- FPS";
                    fpsView.setText(rate);
                } catch (UnsatisfiedLinkError e) { fpsView.setText("FPS indisponível"); }
                hudHandler.postDelayed(this, 500);
            }
        }
    };

    private File root() { return new File(Environment.getExternalStorageDirectory(), ROOT_NAME); }
    private int dp(float n) { return Math.round(n * getResources().getDisplayMetrics().density); }

    private GradientDrawable fpsBackground() {
        GradientDrawable d = new GradientDrawable();
        d.setColor(Color.argb(188, 10, 11, 14));
        d.setCornerRadius(dp(12));
        d.setStroke(dp(1), Color.argb(205, 230, 174, 67));
        return d;
    }

    @Override protected String[] getLibraries() { return new String[] {"SDL3", "main"}; }

    // An immutable launch snapshot crosses the process boundary. Preferences
    // remain a fallback for direct launches, not the source of two divergent
    // copies of the selected CPU/FPS/driver settings.
    private Bundle launchSettings() {
        return getIntent() == null ? null : getIntent().getBundleExtra("defjam_launch");
    }
    private int configuredInt(String key, int fallback) {
        Bundle b = launchSettings();
        return b != null && b.containsKey(key) ? b.getInt(key, fallback)
                : getSharedPreferences("launcher", MODE_PRIVATE).getInt(key, fallback);
    }
    private boolean configuredBool(String key, boolean fallback) {
        Bundle b = launchSettings();
        return b != null && b.containsKey(key) ? b.getBoolean(key, fallback)
                : getSharedPreferences("launcher", MODE_PRIVATE).getBoolean(key, fallback);
    }
    private String configuredString(String key, String fallback) {
        Bundle b = launchSettings();
        return b != null && b.containsKey(key) ? b.getString(key, fallback)
                : getSharedPreferences("launcher", MODE_PRIVATE).getString(key, fallback);
    }

    @Override protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        args.add("--root=" + root().getAbsolutePath());
        args.add("--no-launcher");
        // Fixed compatibility profile requested for the mobile app. Native
        // Auto preserves all CPUs Android permits; old preferences cannot
        // silently re-enable experimental guest multicore on direct launch.
        args.add("--guest-cores=1");
        args.add("--native-cores=0");
        args.add("--vk-async=" + (configuredBool("async_render", true) ? "1" : "0"));
        args.add("--vertex-threads=" + (configuredBool("parallel_vertices", true)
                ? 8 : 1));
        int fps = configuredInt("fps_limit", 60);
        args.add("--framegen=0");
        args.add("--fps-limit=" + (fps >= 30 && fps <= 165 ? fps : 60));
        String library = configuredString("driver_library", "");
        File driverDir = new File(getFilesDir(), "gpu_driver/current");
        if (configuredBool("use_custom_driver", false)
                && !library.isEmpty() && new File(driverDir, library).isFile()) {
            args.add("--driver-dir=" + driverDir.getAbsolutePath() + File.separator);
            args.add("--driver-lib=" + library);
            args.add("--hook-dir=" + getApplicationInfo().nativeLibraryDir);
        }
        return args.toArray(new String[0]);
    }

    private void forceLandscape() {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override protected void onCreate(Bundle state) {
        forceLandscape();
        new File(root(), "game").mkdirs();
        super.onCreate(state);
        forceLandscape();
        boolean showControls = configuredBool("touch_controls", true);
        boolean showFps = configuredBool("show_fps", true);
        if (showControls) {
            controls = new VirtualControlsView(this);
            controls.setMenuOnly(true);
            addContentView(controls, new ViewGroup.LayoutParams(-1, -1));
            menuControls = new XboxMenuControlsView(this);
            addContentView(menuControls, new ViewGroup.LayoutParams(-1, -1));
        }
        portTouch = new PortTouchView(this);
        portTouch.setVisibility(View.GONE);
        addContentView(portTouch, new ViewGroup.LayoutParams(-1, -1));
        // The custom wrench is the editor shortcut in every context. Its
        // menu-only view passes every other touch to the menu/port below.
        if (controls != null) controls.bringToFront();
        if (showFps) {
            fpsView = new TextView(this); fpsView.setText("-- FPS"); fpsView.setTextColor(Color.WHITE);
            fpsView.setTextSize(11); fpsView.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
            fpsView.setGravity(Gravity.CENTER); fpsView.setPadding(dp(12), dp(5), dp(12), dp(5));
            fpsView.setBackground(fpsBackground());
            FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.RIGHT);
            p.setMargins(0, dp(10), 0, 0); addContentView(fpsView, p); hudHandler.post(fpsUpdate);
        }
        installQuickBar();
        hudHandler.post(inputUpdate);
    }

    private void updateInputMode() {
        if (editor != null || portTouch == null) return;
        int automatic;
        try { automatic = nativeGetInputMode(); }
        catch (UnsatisfiedLinkError error) { automatic = 0; }
        int mode = automatic;
        if (mode == appliedInputMode) return;
        appliedInputMode = mode;
        releaseStart();
        if (controls != null) {
            controls.releaseAll(); controls.setMenuOnly(mode != 1); controls.setVisibility(View.VISIBLE);
        }
        if (startButton != null) startButton.setText(mode == 1 ? "START" : "ENTER");
        if (menuControls != null) { menuControls.releaseAll(); menuControls.setVisibility(mode == 0 ? View.VISIBLE : View.GONE); }
        portTouch.releaseTouch(); portTouch.setVisibility(mode == 2 ? View.VISIBLE : View.GONE);
    }

    private android.widget.Button quickButton(String label, Runnable action) {
        android.widget.Button b = new android.widget.Button(this);
        b.setText(label); b.setAllCaps(false); b.setTextSize(11);
        b.setTextColor(Color.WHITE); b.setPadding(dp(10), 0, dp(10), 0);
        b.setMinWidth(0); b.setMinimumWidth(0); b.setBackground(fpsBackground());
        b.setOnClickListener(v -> action.run());
        android.widget.LinearLayout.LayoutParams p = new android.widget.LinearLayout.LayoutParams(-2, dp(36));
        p.setMargins(dp(3), 0, dp(3), 0); quickBar.addView(b, p);
        return b;
    }

    private void installQuickBar() {
        quickBar = new android.widget.LinearLayout(this);
        quickBar.setOrientation(android.widget.LinearLayout.HORIZONTAL);
        startButton = quickButton("ENTER", () -> {
            if (controls != null) controls.releaseAll();
            if (startHeld) return;
            startHeld = true; sendKey(android.view.KeyEvent.KEYCODE_ENTER, true);
            hudHandler.postDelayed(this::releaseStart, 120);
        });
        FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        p.setMargins(dp(8), 0, dp(8), dp(8)); addContentView(quickBar, p);
    }

    private void sendKey(int key, boolean down) {
        long now = android.os.SystemClock.uptimeMillis();
        dispatchKeyEvent(new android.view.KeyEvent(now, now,
                down ? android.view.KeyEvent.ACTION_DOWN : android.view.KeyEvent.ACTION_UP, key, 0));
    }
    private void releaseStart() {
        if (startHeld) { sendKey(android.view.KeyEvent.KEYCODE_ENTER, false); startHeld = false; }
    }

    public void openControlsEditor() {
        if (editor != null || isFinishing()) return;
        releaseStart();
        if (controls != null) { controls.releaseAll(); controls.setVisibility(View.GONE); }
        if (portTouch != null) { portTouch.releaseTouch(); portTouch.setVisibility(View.GONE); }
        if (menuControls != null) { menuControls.releaseAll(); menuControls.setVisibility(View.GONE); }
        quickBar.setVisibility(View.GONE);
        if (fpsView != null) fpsView.setVisibility(View.GONE);
        editor = new ControlsEditorPanel(this, () -> closeControlsEditor(true), () -> closeControlsEditor(false));
        addContentView(editor, new ViewGroup.LayoutParams(-1, -1));
        android.widget.Toast.makeText(this, "Pause pelo START antes de editar; o jogo continua ao fundo.", android.widget.Toast.LENGTH_LONG).show();
    }
    private void closeControlsEditor(boolean saved) {
        if (editor == null) return;
        ((ViewGroup)editor.getParent()).removeView(editor); editor = null;
        if (saved && controls != null) {
            ((ViewGroup)controls.getParent()).removeView(controls);
            controls = new VirtualControlsView(this);
            addContentView(controls, new ViewGroup.LayoutParams(-1, -1));
        }
        if (controls != null) controls.setVisibility(View.VISIBLE);
        quickBar.setVisibility(View.VISIBLE); quickBar.bringToFront();
        if (fpsView != null) { fpsView.setVisibility(View.VISIBLE); fpsView.bringToFront(); }
        appliedInputMode = -1; updateInputMode();
        hideBars();
    }
    @Override public void onBackPressed() {
        if (editor != null) { closeControlsEditor(false); return; }
        super.onBackPressed();
    }

    @Override protected void onResume() {
        super.onResume();
        forceLandscape();
        hudHandler.postDelayed(this::forceLandscape, 250);
    }

    @Override protected void onPause() {
        releaseStart();
        sendKey(android.view.KeyEvent.KEYCODE_F1, false);
        if (controls != null) controls.releaseAll();
        if (portTouch != null) portTouch.releaseTouch();
        if (menuControls != null) menuControls.releaseAll();
        super.onPause();
    }

    @Override protected void onDestroy() {
        hudHandler.removeCallbacksAndMessages(null); fpsView = null;
        if (controls != null) controls.releaseAll();
        super.onDestroy();
        // The game has its own process. Ending it guarantees the next launch
        // can switch cleanly between the system Vulkan loader and Turnip.
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused) hideBars();
        else {
            releaseStart();
            if (menuControls != null) menuControls.releaseAll();
            if (controls != null) controls.releaseAll();
                if (portTouch != null) portTouch.releaseTouch();
        }
    }

    @SuppressWarnings("deprecation") private void hideBars() {
        if (Build.VERSION.SDK_INT < 30) {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE); return;
        }
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) { c.hide(WindowInsets.Type.systemBars()); c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE); }
    }
}
