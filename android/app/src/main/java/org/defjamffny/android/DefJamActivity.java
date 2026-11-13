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
    private FrameHudView fpsView;
    private boolean activityPaused;
    private long monitorLast;
    private VirtualControlsView controls;
    private ControlsEditorPanel editor;
    private OnlinePanel online;
    private VoiceChat133 voice133;
    private OnlineMenuEntry137 menuEntry137;
    private static native int nativeOnlineEntryState137();
    private FrameLayout quickBar;
    private MenuTouchSettings shortcutSettings;
    private boolean startHeld;
    private android.widget.Button startButton;
    private XboxMenuControlsView menuControls;
    private PortTouchView portTouch;
    private int appliedInputMode = -1;
    private boolean appliedGamepad;
    private boolean appliedIdleHidden, appliedOnline, appliedOnlineMenu;
    private final java.util.Set<Integer> nativeMenuHeld = new java.util.HashSet<>();
    private final TouchIdleState touchIdle = new TouchIdleState();
    private android.hardware.input.InputManager inputManager;
    private final GamepadTouchState padTouch = new GamepadTouchState();
    private final InputModeDebounce35 inputMode35 = new InputModeDebounce35();
    private final android.hardware.input.InputManager.InputDeviceListener padListener =
            new android.hardware.input.InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int id) {
            padTouch.deviceAdded(id, isPhysicalGamepad(android.view.InputDevice.getDevice(id)));
            touchIdle.reset(android.os.SystemClock.uptimeMillis());
            refreshGamepadVisibility();
        }
        @Override public void onInputDeviceRemoved(int id) {
            padTouch.deviceRemoved(id);
            touchIdle.reset(android.os.SystemClock.uptimeMillis());
            refreshGamepadVisibility();
        }
        @Override public void onInputDeviceChanged(int id) { touchIdle.reset(android.os.SystemClock.uptimeMillis()); refreshGamepadVisibility(); }
    };
    private static boolean isPhysicalGamepad(android.view.InputDevice device) {
        return device != null && !device.isVirtual()
                && (device.supportsSource(android.view.InputDevice.SOURCE_GAMEPAD)
                || device.supportsSource(android.view.InputDevice.SOURCE_JOYSTICK));
    }
    private boolean hasGamepad() {
        return padTouch.shouldHideTouch(android.view.InputDevice.getDeviceIds(),
                id -> isPhysicalGamepad(android.view.InputDevice.getDevice(id)));
    }
    private void refreshGamepadVisibility() {
        appliedInputMode = -1;
        updateInputMode();
    }

    @Override public boolean dispatchTouchEvent(android.view.MotionEvent event) {
        if (event.isFromSource(android.view.InputDevice.SOURCE_TOUCHSCREEN)) {
            int action=event.getActionMasked();
            int fingers=action==android.view.MotionEvent.ACTION_UP||action==android.view.MotionEvent.ACTION_CANCEL?0:
                    event.getPointerCount()-(action==android.view.MotionEvent.ACTION_POINTER_UP?1:0);
            boolean wasHidden=touchIdle.hidden();
            touchIdle.touch(android.os.SystemClock.uptimeMillis(),fingers);
            if(wasHidden)updateInputMode();
        }
        if (appliedGamepad && event.getActionMasked() == android.view.MotionEvent.ACTION_DOWN
                && event.isFromSource(android.view.InputDevice.SOURCE_TOUCHSCREEN)) {
            padTouch.requestTouch();
            refreshGamepadVisibility();
        }
        // The same first finger press reaches the restored touch controls/editor.
        return super.dispatchTouchEvent(event);
    }

    @Override public boolean dispatchKeyEvent(android.view.KeyEvent event) {
        if(event.getAction()==android.view.KeyEvent.ACTION_DOWN&&event.getRepeatCount()==0&&
           (event.getKeyCode()==android.view.KeyEvent.KEYCODE_ENTER||event.getKeyCode()==android.view.KeyEvent.KEYCODE_BUTTON_START))
            inputMode35.pauseIntent(android.os.SystemClock.uptimeMillis());
        if (event.getAction() == android.view.KeyEvent.ACTION_DOWN
                && event.getRepeatCount() == 0 && isPhysicalGamepad(event.getDevice())) {
            int key = event.getKeyCode();
            if (android.view.KeyEvent.isGamepadButton(key)
                    || key >= android.view.KeyEvent.KEYCODE_DPAD_UP && key <= android.view.KeyEvent.KEYCODE_DPAD_CENTER
                    || key == android.view.KeyEvent.KEYCODE_ENTER || key == android.view.KeyEvent.KEYCODE_BACK) {
                if (padTouch.useGamepad(event.getDeviceId())) updateInputMode();
            }
        }
        return super.dispatchKeyEvent(event);
    }

    @Override public boolean dispatchGenericMotionEvent(android.view.MotionEvent event) {
        android.view.InputDevice device = event.getDevice();
        if (isPhysicalGamepad(device) && intentionalPadMotion(event, device)) {
            if (padTouch.useGamepad(event.getDeviceId())) updateInputMode();
        }
        return super.dispatchGenericMotionEvent(event);
    }
    private static boolean intentionalPadMotion(android.view.MotionEvent event, android.view.InputDevice device) {
        if (event.getActionMasked() != android.view.MotionEvent.ACTION_MOVE) return false;
        for (int axis : new int[] {android.view.MotionEvent.AXIS_X, android.view.MotionEvent.AXIS_Y,
                android.view.MotionEvent.AXIS_Z, android.view.MotionEvent.AXIS_RZ,
                android.view.MotionEvent.AXIS_HAT_X, android.view.MotionEvent.AXIS_HAT_Y}) {
            android.view.InputDevice.MotionRange range = device.getMotionRange(axis, event.getSource());
            float threshold = range == null ? .25f : Math.max(.25f, range.getFlat());
            if (Math.abs(event.getAxisValue(axis)) > threshold) return true;
        }
        for (int axis : new int[] {android.view.MotionEvent.AXIS_LTRIGGER, android.view.MotionEvent.AXIS_RTRIGGER,
                android.view.MotionEvent.AXIS_BRAKE, android.view.MotionEvent.AXIS_GAS}) {
            if (event.getAxisValue(axis) > .35f) return true;
        }
        return false;
    }
    private static native void nativeTouchSticks(int lx,int ly,int rx,int ry);
    void sendTouchSticks(int lx,int ly,int rx,int ry){nativeTouchSticks(lx,ly,rx,ry);}
    private static native int nativeGetInputMode();
    private static native boolean nativeOnlineMenuButton(int button, boolean down);

    void sendMenuKey(int key, boolean down) {
        int button;
        switch(key) {
            case android.view.KeyEvent.KEYCODE_NUMPAD_8: button=0;break;
            case android.view.KeyEvent.KEYCODE_NUMPAD_2: button=1;break;
            case android.view.KeyEvent.KEYCODE_NUMPAD_4: button=2;break;
            case android.view.KeyEvent.KEYCODE_NUMPAD_6: button=3;break;
            case android.view.KeyEvent.KEYCODE_K:
            case android.view.KeyEvent.KEYCODE_ENTER: button=4;break;
            case android.view.KeyEvent.KEYCODE_L: button=5;break;
            case android.view.KeyEvent.KEYCODE_J: button=6;break;
            case android.view.KeyEvent.KEYCODE_I: button=7;break;
            case android.view.KeyEvent.KEYCODE_Q: button=8;break;
            case android.view.KeyEvent.KEYCODE_E: button=9;break;
            default: button=-1;
        }
        boolean owned=!down&&nativeMenuHeld.remove(key);
        try {
            if(button>=0&&nativeOnlineMenuButton(button,down)) {
                if(down)nativeMenuHeld.add(key);
                return;
            }
        } catch(UnsatisfiedLinkError ignored) { }
        if(owned)return; // Online closed between down/up: don't release an unrelated guest key.
        long now=android.os.SystemClock.uptimeMillis();
        dispatchKeyEvent(new android.view.KeyEvent(now,now,down?android.view.KeyEvent.ACTION_DOWN:android.view.KeyEvent.ACTION_UP,key,0));
    }
    private static native float nativeGetFps();
    private static native float[] nativeFrameStats();
    private static native void nativeRuntimeStats(long collections,long gcMs,long bytes);
    private long lastRuntime123;
    private void reportRuntime123(){
        long now=android.os.SystemClock.uptimeMillis();if(now-lastRuntime123<10000)return;lastRuntime123=now;
        try{nativeRuntimeStats(Long.parseLong(android.os.Debug.getRuntimeStat("art.gc.gc-count")),Long.parseLong(android.os.Debug.getRuntimeStat("art.gc.gc-time")),Long.parseLong(android.os.Debug.getRuntimeStat("art.gc.bytes-allocated")));}
        catch(Exception|LinkageError unavailable){try{nativeRuntimeStats(-1,-1,-1);}catch(LinkageError ignored){}}
    }
    private static native int nativePowerState25();
    static native void nativePowerDisplay25(int requested,int milliHz,int mode,int sustained);
    private PowerPolicy25 powerPolicy25;
    private long lastPower25;
    private void pollPower25(){
        long now=android.os.SystemClock.uptimeMillis();if(now-lastPower25<1000||activityPaused)return;lastPower25=now;
        try{if(powerPolicy25==null)powerPolicy25=new PowerPolicy25(this);powerPolicy25.apply(nativePowerState25());}
        catch(RuntimeException|LinkageError unavailable){android.util.Log.w("POWER25","Power request unavailable",unavailable);}
    }
    private static native void nativeMonitorState(boolean paused,int thermal);
    private void monitorState(boolean paused) {
        android.os.PowerManager power=(android.os.PowerManager)getSystemService(POWER_SERVICE);
        int status=power==null?-1:power.getCurrentThermalStatus();
        try{nativeMonitorState(paused,status);}catch(UnsatisfiedLinkError ignored){}
    }
    private static native boolean nativeCanOpenOnline();
    private static native boolean nativeOpenOnline(String server,String name);
    static native boolean nativeOnlineSession();
    void openNativeOnline(){
        try {
            if(!nativeCanOpenOnline()){
                android.widget.Toast.makeText(this,UiText.t("Open Online from the game's main menu."),android.widget.Toast.LENGTH_SHORT).show();
                return;
            }
            // Returning to a live room must not rename, reset or reconnect it.
            if(nativeOnlineSession()){
                openOnlineWithName129(OnlineAccount133.gameName());
                return;
            }
            OnlineAccount133.enter(this, () -> openOnlineWithName129(OnlineAccount133.gameName()));
        } catch(UnsatisfiedLinkError unavailable){
            android.widget.Toast.makeText(this,UiText.t("Online native module is unavailable in this APK."),android.widget.Toast.LENGTH_SHORT).show();
        }
    }
    private boolean openOnlineWithName129(String player){
        if(!nativeOpenOnline(OnlineMenuState.DEFAULT_SERVER, player)){
            android.widget.Toast.makeText(this,UiText.t("Open Online from the game's main menu."),android.widget.Toast.LENGTH_SHORT).show();
            return false;
        }
        return true;
    }
    private static native float nativeGetGeneratedFps();

    private final Runnable inputUpdate = new Runnable() {
        @Override public void run() {
            if (!isFinishing()) {
                long stamp=android.os.SystemClock.uptimeMillis();
                if(stamp-monitorLast>=10000){monitorState(activityPaused||!hasWindowFocus());monitorLast=stamp;}
                if(online!=null)online.pollMenuRequest();
                if(menuEntry137!=null){
                    try{menuEntry137.update(editor==null?nativeOnlineEntryState137():0,nativeOnlineSession(),!activityPaused&&hasWindowFocus(),stamp);}
                    catch(UnsatisfiedLinkError missing){menuEntry137.hide();}
                }
                if(editor==null&&hasWindowFocus()) {
                    long now=android.os.SystemClock.uptimeMillis();
                    if(online!=null&&online.isSessionRequested())touchIdle.keepVisible(now);
                    else touchIdle.advance(now);
                }
                pollGraphics145();
                pollPower25();
                updateInputMode();
                hudHandler.postDelayed(this, 100);
            }
        }
    };

    private final Runnable fpsUpdate = new Runnable() {
        @Override public void run() {
            if (fpsView != null) {
                try {
                    reportRuntime123();fpsView.update(nativeFrameStats());
                } catch (UnsatisfiedLinkError ignored) { }
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

    private static native void nativeSetOnlineArtRoot(String root);
    @Override protected String[] getArguments() {
        // Libraries are loaded by SDL before arguments; prepare only before game boot.
        try { GraphicsMenu147.ensure(root()); android.util.Log.i("GRAPHICS147","Original Options GRAPHICS ready"); }
        catch(java.io.IOException|RuntimeException ex) { android.util.Log.w("GRAPHICS147","Original Options menu preparation failed; original archive retained",ex); }
        try { OnlineArtInstaller.ensure(this,root()); }
        catch(java.io.IOException|RuntimeException ex) { android.util.Log.w("ONLINE-ART126","Artwork remains optional; retry on next launch",ex); }
        nativeSetOnlineArtRoot(OnlineArtInstaller.directory(this).getAbsolutePath());
        List<String> args = new ArrayList<>();
        args.add("--root=" + root().getAbsolutePath());
        args.add("--no-launcher");
        // Fixed compatibility profile requested for the mobile app. Native
        // The native renderer uses the separate CPU profile; old preferences cannot
        // silently re-enable experimental guest multicore on direct launch.
        args.add("--guest-cores=1");
        args.add("--native-cores=0");
        args.add("--native-layout=split");
        args.add("--legacy-fxaa145=" + (configuredBool("fxaa", false) ? "1" : "0"));
        // The default stays asynchronous; only an explicit compatibility choice serializes frames.
        int gpu136=GpuCompatibility136.valid(configuredInt("gpu_compat136",0));
        args.add("--vk-async="+(GpuCompatibility136.serial(gpu136)?"0":"1"));
        if(GpuCompatibility136.conservative(gpu136))args.add("--render-fast=0");
        args.add("--vertex-threads=2");
        args.add("--framegen=0"); // Frame limit now comes from the in-game settings.ini.
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
        CrashReport35.install(this);
        UiText.setLanguage(configuredString("ui_language", "en"));
        forceLandscape();
        new File(root(), "game").mkdirs();
        super.onCreate(state);
        OnlineAccount133.init(this,true);
        voice133=new VoiceChat133(this);
        menuEntry137=new OnlineMenuEntry137(this);
        touchIdle.reset(android.os.SystemClock.uptimeMillis());
        forceLandscape();
        boolean showControls = true; // Editor shortcut is always available; pads hide gameplay touch.
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
        {
            fpsView = new FrameHudView(this);
            fpsView.setVisibility(showFps?View.VISIBLE:View.GONE);
            addContentView(fpsView, fpsLayout(0, false));
            hudHandler.post(fpsUpdate);
        }
        installQuickBar();
        online = new OnlinePanel(this);
        if(getIntent().getBooleanExtra("open_online",false))online.showWhenReady();
        applyStartStyle();
        inputManager = (android.hardware.input.InputManager)getSystemService(INPUT_SERVICE);
        if (inputManager != null) inputManager.registerInputDeviceListener(padListener, hudHandler);
        updateInputMode();
        hudHandler.post(inputUpdate);
    }

    private void updateInputMode() {
        if (editor != null || portTouch == null) return;
        int automatic;
        try { automatic = nativeGetInputMode(); }
        catch (UnsatisfiedLinkError error) { automatic = 0; }
        automatic=inputMode35.choose(automatic,android.os.SystemClock.uptimeMillis());
        int mode = TouchLayoutMode.resolve(automatic, controls == null ? 0 : controls.getLayoutMode());
        boolean onlineMenu=automatic==3;
        boolean pad = hasGamepad();
        boolean connected=online!=null&&online.isSessionRequested();
        boolean idle=touchIdle.hidden()&&!connected&&!onlineMenu;
        if (mode == appliedInputMode && pad == appliedGamepad && idle == appliedIdleHidden && connected == appliedOnline && onlineMenu==appliedOnlineMenu) return;
        appliedInputMode = mode;
        appliedGamepad = pad;
        appliedIdleHidden = idle;
        appliedOnline = connected;
        appliedOnlineMenu = onlineMenu;
        android.util.Log.i("DefJamControls", "mode=" + mode + " gamepad=" + pad
                + " touch=" + (pad || idle ? "hidden" : "visible"));
        releaseStart();
        if (controls != null) {
            controls.releaseAll(); controls.setMenuOnly(pad || mode != 1); controls.setVisibility(idle ? View.INVISIBLE : View.VISIBLE);
        }
        // Only START remains as a floating shortcut; ONLINE is in the game menu.
        if (quickBar != null) quickBar.setVisibility(View.VISIBLE);
        if (startButton != null) {
            startButton.setVisibility(!pad&&!idle&&mode!=2?View.VISIBLE:View.GONE);
            startButton.setText(mode == 1 ? "START" : "ENTER");
        }
        if (menuControls != null) { menuControls.releaseAll(); menuControls.setOnlineMode(onlineMenu); menuControls.setVisibility(!pad && mode == 0 ? (idle ? View.INVISIBLE : View.VISIBLE) : View.GONE); }
        portTouch.releaseTouch(); portTouch.setVisibility(!pad && mode == 2 ? (idle ? View.INVISIBLE : View.VISIBLE) : View.GONE);
    }

    private android.widget.Button quickButton(String label, Runnable action) {
        android.widget.Button b = new android.widget.Button(this);
        b.setText(label); b.setAllCaps(false); b.setTextSize(11);
        b.setTextColor(Color.WHITE); b.setPadding(dp(10), 0, dp(10), 0);
        b.setMinWidth(0); b.setMinimumWidth(0); b.setBackground(fpsBackground());
        b.setOnClickListener(v -> action.run());
        FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(dp(80), dp(36),Gravity.TOP|Gravity.LEFT);
        quickBar.addView(b, p);
        return b;
    }

    private void installQuickBar() {
        quickBar = new FrameLayout(this);
        quickBar.addOnLayoutChangeListener((v,l,t,r,b,ol,ot,or,ob)->{
            if(r-l!=or-ol||b-t!=ob-ot)layoutQuickButtons();
        });
        startButton = quickButton("ENTER", () -> {
            if (controls != null) controls.releaseAll();
            if (startHeld) return;
            startHeld = true; sendKey(android.view.KeyEvent.KEYCODE_ENTER, true);
            hudHandler.postDelayed(this::releaseStart, 120);
        });
        addContentView(quickBar, new ViewGroup.LayoutParams(-1,-1));
    }

    private void applyStartStyle() {
        shortcutSettings=MenuTouchSettings.read(this);
        layoutQuickButtons();
    }
    private void layoutQuickButtons() {
        if(quickBar==null||shortcutSettings==null)return;
        int w=quickBar.getWidth(),h=quickBar.getHeight();
        if(w<=0||h<=0)return;
        android.widget.Button[] buttons={startButton};
        for(int n=0;n<buttons.length;n++) {
            android.widget.Button button=buttons[n];if(button==null)continue;
            int index=MenuTouchSettings.START+n;
            float scale=shortcutSettings.size[index];
            int width=Math.min(w,dp(80*scale)),height=Math.min(h,dp(36*scale));
            FrameLayout.LayoutParams p=new FrameLayout.LayoutParams(width,height,Gravity.TOP|Gravity.LEFT);
            p.leftMargin=Math.round(MenuTouchSettings.center(shortcutSettings.x[index],w,width/2f)-width/2f);
            p.topMargin=Math.round(MenuTouchSettings.center(shortcutSettings.y[index],h,height/2f)-height/2f);
            button.setLayoutParams(p);button.setTextSize(11*scale);
            button.setAlpha(shortcutSettings.alpha[index]);
        }
    }

    private void sendKey(int key, boolean down) {
        sendMenuKey(key, down);
    }
    private void releaseStart() {
        if (startHeld) { sendKey(android.view.KeyEvent.KEYCODE_ENTER, false); startHeld = false; }
    }
    void restoreAfterOnline(){touchIdle.reset(android.os.SystemClock.uptimeMillis());appliedInputMode=-1;updateInputMode();hideBars();}

    void openVoiceSettings133(){if(voice133!=null)voice133.settings();}

    private static native void nativeOpenPort145();
    private static native int nativeGraphicsState145(int legacyHud,int language);
    private int graphicsHudLayout = -1;
    private FrameLayout.LayoutParams fpsLayout(int position, boolean classic) {
        int gravity = Gravity.TOP | (position == 1 ? Gravity.CENTER_HORIZONTAL
                : position == 2 ? Gravity.LEFT : Gravity.RIGHT);
        FrameLayout.LayoutParams layout = new FrameLayout.LayoutParams(
                dp(classic ? 205 : 88), dp(classic ? 54 : 21), gravity);
        int edge = classic ? 0 : dp(6);
        layout.setMargins(edge, dp(classic ? 10 : 6), edge, 0);
        return layout;
    }
    public void openPortMenu145(){
        if(editor!=null||isFinishing())return;
        touchIdle.reset(android.os.SystemClock.uptimeMillis());releaseStart();
        if(controls!=null)controls.releaseAll();if(menuControls!=null)menuControls.releaseAll();
        try{nativeOpenPort145();}catch(UnsatisfiedLinkError unavailable){openControlsEditor();}
    }
    private boolean restartDialog148;
    private void showGraphicsRestart148(int mask){
        if(restartDialog148||isFinishing())return;restartDialog148=true;android.util.Log.i("GRAPHICS148","Restart prompt options mask="+mask);
        String[] labels={"Resolução interna","Formato da tela","","Sincronização vertical","","","","","","","","Filtro anisotrópico","","","","Pré-carregar shaders","Sincronização Vulkan","Pacotes de textura","Upscaling / geração de quadros"};
        if(!UiText.isPortuguese())labels=UiText.isSpanish()?new String[]{"Resolución","Formato","","VSync","","","","","","","","Filtro anisotrópico","","","","Precarga de shaders","Sincronización Vulkan","Texturas","Escala / generación de cuadros"}:new String[]{"Render resolution","Aspect ratio","","VSync","","","","","","","","Anisotropic filtering","","","","Shader preloading","Vulkan synchronization","Texture packs","Upscaling / frame generation"};
        StringBuilder changed=new StringBuilder();for(int i=0;i<labels.length;i++)if((mask&(1<<i))!=0)changed.append("• ").append(labels[i]).append("\n");
        android.app.AlertDialog choice148=PortUi.dialog(this).setTitle(UiText.tr("Restart the game?","Reiniciar o jogo?","¿Reiniciar el juego?"))
            .setMessage(changed+UiText.tr("\nThese settings are saved and require a restart. Restarting ends the current match.","\nEssas opções foram salvas e precisam reiniciar o jogo para entrar em efeito. Reiniciar encerra a partida atual.","\nLas opciones se guardaron y requieren reiniciar. Reiniciar termina la partida actual."))
            .setPositiveButton(UiText.tr("Restart now","Reiniciar agora","Reiniciar ahora"),(d,w)->{
                android.content.Intent launch=new android.content.Intent(this,LauncherActivity.class);
                launch.putExtra("restart_game148",true);launch.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK|android.content.Intent.FLAG_ACTIVITY_CLEAR_TASK);
                startActivity(launch);finish();
            }).setNegativeButton(UiText.tr("Later","Depois","Después"),null).setOnDismissListener(d->restartDialog148=false).create();
        choice148.show();
        choice148.getButton(android.app.AlertDialog.BUTTON_NEGATIVE).requestFocus();
        choice148.getButton(android.app.AlertDialog.BUTTON_NEGATIVE).setTextColor(PortUi.GOLD);
        choice148.getButton(android.app.AlertDialog.BUTTON_POSITIVE).setTextColor(PortUi.GOLD);
    }
    private void pollGraphics145(){
        try{
            int state=nativeGraphicsState145(configuredBool("show_fps",true)?1:0,UiText.isPortuguese()?1:UiText.isSpanish()?2:0);
            if((state&512)!=0){releaseStart();if(controls!=null)controls.releaseAll();if(menuControls!=null)menuControls.releaseAll();}
            if((state>>>12)!=0)showGraphicsRestart148(state>>>12);
            if((state&256)!=0&&editor==null)openControlsEditor();
            if((state&1024)!=0)startActivity(new android.content.Intent(this,FramegenActivity29.class));
            if(fpsView==null)return;
            int visibility=(state&1)!=0&&editor==null?View.VISIBLE:View.GONE;
            if(fpsView.getVisibility()!=visibility)fpsView.setVisibility(visibility);
            int layout = state & 0x0e;
            if (layout != graphicsHudLayout) {
                graphicsHudLayout = layout;
                boolean classic = (layout & 8) != 0;
                fpsView.setClassic(classic);
                fpsView.setLayoutParams(fpsLayout((layout >> 1) & 3, classic));
            }
        }catch(UnsatisfiedLinkError ignored){}
    }

    public void openControlsEditor() {
        if (editor != null || isFinishing()) return;
        touchIdle.reset(android.os.SystemClock.uptimeMillis());
        releaseStart();
        if (controls != null) { controls.releaseAll(); controls.setVisibility(View.GONE); }
        if (portTouch != null) { portTouch.releaseTouch(); portTouch.setVisibility(View.GONE); }
        if (menuControls != null) { menuControls.releaseAll(); menuControls.setVisibility(View.GONE); }
        quickBar.setVisibility(View.GONE);
        if (fpsView != null) fpsView.setVisibility(View.GONE);
        editor = new ControlsEditorPanel(this, () -> closeControlsEditor(true), () -> closeControlsEditor(false));
        addContentView(editor, new ViewGroup.LayoutParams(-1, -1));

    }
    private void closeControlsEditor(boolean saved) {
        if (editor == null) return;
        ((ViewGroup)editor.getParent()).removeView(editor); editor = null;
        if (saved && controls != null) {
            ((ViewGroup)controls.getParent()).removeView(controls);
            controls = new VirtualControlsView(this);
            addContentView(controls, new ViewGroup.LayoutParams(-1, -1));
            if (menuControls != null) ((ViewGroup)menuControls.getParent()).removeView(menuControls);
            menuControls = new XboxMenuControlsView(this);
            addContentView(menuControls, new ViewGroup.LayoutParams(-1, -1));
            controls.bringToFront();
            applyStartStyle();
        }
        if (controls != null) controls.setVisibility(View.VISIBLE);
        quickBar.setVisibility(View.VISIBLE); quickBar.bringToFront();
        if (fpsView != null) fpsView.bringToFront();
        touchIdle.reset(android.os.SystemClock.uptimeMillis());appliedInputMode = -1; updateInputMode();
        hideBars();
    }
    @Override public void onBackPressed() {
        if (editor != null) { closeControlsEditor(false); return; }
        try{if(nativeGetInputMode()==2){sendKey(android.view.KeyEvent.KEYCODE_ESCAPE,true);hudHandler.postDelayed(()->sendKey(android.view.KeyEvent.KEYCODE_ESCAPE,false),60);return;}}catch(UnsatisfiedLinkError unavailable){}
        super.onBackPressed();
    }

    @Override protected void onResume() {
        super.onResume();
        OnlineAccount133.gameForeground(true);
        if(voice133!=null)voice133.resume();
        activityPaused=false;
        monitorState(false);
        // A launcher selection must also apply when returning to an existing
        // game Activity. Replace only the fight overlay, never SDL's surface.
        if(controls!=null&&editor==null){
            try {
                int style=new FightControlProfiles(FightControlProfiles.read(this)).style();
                if(style!=controls.getFightStyle()){
                    controls.releaseAll();((ViewGroup)controls.getParent()).removeView(controls);
                    controls=new VirtualControlsView(this);addContentView(controls,new ViewGroup.LayoutParams(-1,-1));
                    controls.bringToFront();if(quickBar!=null)quickBar.bringToFront();if(fpsView!=null)fpsView.bringToFront();
                    appliedInputMode=-1;
                }
            }catch(Exception error){android.util.Log.w("DefJamControls","Saved fight profile could not be refreshed",error);}
        }
        touchIdle.reset(android.os.SystemClock.uptimeMillis());
        refreshGamepadVisibility();
        forceLandscape();
        hudHandler.postDelayed(this::forceLandscape, 250);
    }

    @Override protected void onPause() {
        OnlineAccount133.gameForeground(false);
        if(menuEntry137!=null)menuEntry137.hide();
        if(voice133!=null)voice133.pause();
        activityPaused=true;
        if(powerPolicy25!=null)powerPolicy25.reset();
        monitorState(true);
        touchIdle.reset(android.os.SystemClock.uptimeMillis());
        if(online!=null)online.suspend();
        releaseStart();
        sendKey(android.view.KeyEvent.KEYCODE_F1, false);
        if (controls != null) controls.releaseAll();
        if (portTouch != null) portTouch.releaseTouch();
        if (menuControls != null) menuControls.releaseAll();
        super.onPause();
    }

    @Override protected void onDestroy() {
        if(menuEntry137!=null){menuEntry137.destroy();menuEntry137=null;}
        if(voice133!=null)voice133.destroy();
        if(online!=null)online.destroy();
        if (inputManager != null) inputManager.unregisterInputDeviceListener(padListener);
        hudHandler.removeCallbacksAndMessages(null); fpsView = null;
        if (controls != null) controls.releaseAll();
        super.onDestroy();
        // The game has its own process. Ending it guarantees the next launch
        // can switch cleanly between the system Vulkan loader and Turnip.
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused) { touchIdle.reset(android.os.SystemClock.uptimeMillis());appliedInputMode=-1;updateInputMode();hideBars(); }
        else {
            touchIdle.reset(android.os.SystemClock.uptimeMillis());
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
