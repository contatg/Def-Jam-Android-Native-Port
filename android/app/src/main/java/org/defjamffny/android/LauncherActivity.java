package org.defjamffny.android;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public final class LauncherActivity extends Activity {
    private static final int STORAGE_REQUEST = 0x444A;
    private static final int DRIVER_REQUEST = 0x5452;
    private static final long MAX_DRIVER_BYTES = 512L * 1024L * 1024L;
    private static final int MAX_DRIVER_ENTRIES = 256;
    private static final int BG = Color.rgb(10, 12, 17);
    private static final int CARD = Color.rgb(22, 27, 35);
    private static final int CARD_LINE = Color.rgb(49, 58, 72);
    private static final int GOLD = Color.rgb(230, 174, 67);
    private static final int MUTED = Color.rgb(166, 170, 178);
    private static final int GREEN = Color.rgb(102, 211, 145);
    private static final int RED = Color.rgb(238, 111, 103);

    private ChoicePill frameRate;
    private ChoicePill antiAliasing;
    private ChoicePill fsr144, screenFilter144, anisotropy144;
    private TogglePill shaderPreload144;
    private ChoicePill language;
    private ChoicePill scale;
    private ChoicePill aspect;
    private ChoicePill driver;
    private ChoicePill execution34;
    private Button emulatorOptions36;
    private TogglePill vsync;
    private TogglePill smooth;
    private TogglePill postEffects;
    private TogglePill hdTextures;
    private TogglePill showFps;
    private boolean binding;

    private File root() {
        return new File(Environment.getExternalStorageDirectory(), "DefJamFFNY");
    }

    private File driverDir() {
        return new File(getFilesDir(), "gpu_driver/current");
    }

    private SharedPreferences prefs() {
        return getSharedPreferences("launcher", MODE_PRIVATE);
    }

    private int dp(float n) {
        return Math.round(n * getResources().getDisplayMetrics().density);
    }

    private void forceLandscape() {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    private boolean hasStorage() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    @Override
    protected void onCreate(Bundle state) {
        forceLandscape();
        super.onCreate(state);
        UiText.setLanguage(prefs().getString("ui_language", "en"));
        migrateFromBrokenLayout();
        if(!prefs().getBoolean("fg34_settings_migrated",false))prefs().edit().remove("lsfg27_pending").remove("lsfg27_clear_effects").putBoolean("fg34_settings_migrated",true).apply();
        if (!prefs().getBoolean("v13_recovery", false)) {
            prefs().edit().putBoolean("framegen", false).putBoolean("parallel_vertices", false)
                    .putBoolean("v13_recovery", true).commit();
        }
        // Fixed mobile rendering policy. Replace old saved choices on every
        // launcher start; neither setting is exposed as an editable control.
        prefs().edit().putBoolean("async_render", true)
                .putBoolean("parallel_vertices", false).commit();
        showLauncher();
        if(getIntent().getBooleanExtra("restart_game148",false)){
            getIntent().removeExtra("restart_game148");
            new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(()->{if(!isFinishing())play();},1200);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        CrashReport35.collect(this);
        forceLandscape();
        if(hasStorage())OnlineArtInstaller.hideLegacy(this,root());
        if (driver != null) showLauncher();
        GitHubUpdates.onLauncherResume(this);
    }

    private void migrateFromBrokenLayout() {
        SharedPreferences p = prefs();
        if (p.getBoolean("ui_v2_migrated", false)) return;
        p.edit()
                .putBoolean("ui_v2_migrated", true)
                .putInt("render_scale", 1)
                .putBoolean("vsync", false)
                .putInt("control_layout", 1)
                .putInt("control_size", 90)
                .commit();
    }

    private GradientDrawable background(int color, float radius, int stroke) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(radius));
        if (stroke != 0) d.setStroke(dp(1), stroke);
        return d;
    }

    private TextView text(String value, float size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(UiText.t(value));
        t.setTextSize(size);
        t.setTextColor(color);
        t.setGravity(Gravity.CENTER_VERTICAL);
        if (bold) t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return t;
    }

    private LinearLayout card(String title, String subtitle) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(16), dp(12), dp(16), dp(12));
        c.setBackground(background(CARD, 16, CARD_LINE));
        c.addView(text(title, 16, Color.WHITE, true));
        TextView sub = text(subtitle, 11, MUTED, false);
        LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(-1, -2);
        sp.topMargin = dp(2);
        sp.bottomMargin = dp(5);
        c.addView(sub, sp);
        return c;
    }

    private LinearLayout row(String label, View control) {
        LinearLayout r = new LinearLayout(this);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.addView(text(label, 12, Color.WHITE, false), new LinearLayout.LayoutParams(0, dp(43), 0.46f));
        LinearLayout slot = new LinearLayout(this);
        slot.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        int width = control instanceof TogglePill ? dp(104) : LinearLayout.LayoutParams.MATCH_PARENT;
        slot.addView(control, new LinearLayout.LayoutParams(width, dp(36)));
        r.addView(slot, new LinearLayout.LayoutParams(0, dp(43), 0.54f));
        return r;
    }

    private ChoicePill spinner(String... values) {
        return new ChoicePill(values);
    }

    private TogglePill toggle(boolean checked) {
        return new TogglePill(checked);
    }

    private ValueSlider seek(int max, int value, int offset) {
        return new ValueSlider(max, value, offset);
    }

    /** Dark, predictable replacement for the device-themed white Spinner. */
    private final class ChoicePill extends TextView {
        private final String[] values;
        private int selection;
        private Runnable changed;

        ChoicePill(String... values) {
            super(LauncherActivity.this);
            this.values = values.clone();
            for (int i = 0; i < this.values.length; i++) this.values[i] = UiText.t(this.values[i]);
            setTextSize(11);
            setTextColor(Color.WHITE);
            setTypeface(Typeface.DEFAULT, Typeface.BOLD);
            setGravity(Gravity.CENTER_VERTICAL);
            setSingleLine(true);
            setEllipsize(TextUtils.TruncateAt.END);
            setPadding(dp(13), 0, dp(10), 0);
            setBackground(background(Color.rgb(29, 32, 38), 11, Color.rgb(61, 65, 74)));
            setOnClickListener(v -> {
                if (this.values.length > 1) setSelection((selection + 1) % this.values.length);
            });
            refresh();
        }

        void setSelection(int value) {
            int next = Math.max(0, Math.min(values.length - 1, value));
            boolean different = next != selection;
            selection = next;
            refresh();
            if (different && changed != null) changed.run();
        }

        int getSelectedItemPosition() { return selection; }
        void setOnSelectionChanged(Runnable listener) { changed = listener; }

        private void refresh() {
            setText(values[Math.max(0, Math.min(values.length - 1, selection))] + "   ›");
            setAlpha(values.length > 1 ? 1f : .72f);
        }
    }

    /** Compact toggle which keeps the launcher independent of OEM switch themes. */
    private final class TogglePill extends TextView {
        private boolean checked;
        private Runnable changed;

        TogglePill(boolean value) {
            super(LauncherActivity.this);
            setTextSize(10);
            setTypeface(Typeface.DEFAULT, Typeface.BOLD);
            setGravity(Gravity.CENTER);
            setPadding(dp(9), 0, dp(9), 0);
            setOnClickListener(v -> setChecked(!checked));
            setCheckedSilently(value);
        }

        boolean isChecked() { return checked; }
        void setOnCheckedChanged(Runnable listener) { changed = listener; }

        void setChecked(boolean value) {
            boolean different = value != checked;
            setCheckedSilently(value);
            if (different && changed != null) changed.run();
        }

        private void setCheckedSilently(boolean value) {
            checked = value;
            setText(UiText.t(value ? "ON" : "OFF"));
            setTextColor(value ? BG : MUTED);
            setBackground(background(value ? GOLD : Color.rgb(29, 32, 38), 18,
                    value ? 0 : Color.rgb(61, 65, 74)));
        }
    }

    /** Slider plus a fixed-width percentage readout, with consistent colors. */
    private final class ValueSlider extends LinearLayout {
        private final SeekBar bar;
        private final TextView value;
        private final int offset;
        private final String suffix;
        private Runnable changed;

        ValueSlider(int max, int progress, int offset) {
            this(max, progress, offset, "%");
        }

        ValueSlider(int max, int progress, int offset, String suffix) {
            super(LauncherActivity.this);
            this.offset = offset;
            this.suffix = suffix;
            setOrientation(HORIZONTAL);
            setGravity(Gravity.CENTER_VERTICAL);
            bar = new SeekBar(LauncherActivity.this);
            bar.setMax(max);
            bar.setProgress(Math.max(0, Math.min(max, progress)));
            bar.setProgressTintList(ColorStateList.valueOf(GOLD));
            bar.setProgressBackgroundTintList(ColorStateList.valueOf(Color.rgb(62, 66, 74)));
            bar.setThumbTintList(ColorStateList.valueOf(GOLD));
            bar.setPadding(0, 0, dp(8), 0);
            value = text("", 10, Color.WHITE, true);
            value.setGravity(Gravity.CENTER);
            value.setBackground(background(Color.rgb(29, 32, 38), 9, Color.rgb(61, 65, 74)));
            addView(bar, new LinearLayout.LayoutParams(0, -1, 1));
            addView(value, new LinearLayout.LayoutParams(dp(54), dp(30)));
            bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                @Override public void onProgressChanged(SeekBar seekBar, int n, boolean fromUser) {
                    refresh();
                    if (fromUser && changed != null) changed.run();
                }
                @Override public void onStartTrackingTouch(SeekBar seekBar) { }
                @Override public void onStopTrackingTouch(SeekBar seekBar) {
                    if (changed != null) changed.run();
                }
            });
            refresh();
        }

        int getProgress() { return bar.getProgress(); }
        void setOnValueChanged(Runnable listener) { changed = listener; }
        private void refresh() { value.setText((bar.getProgress() + offset) + suffix); }
    }

    private Button button(String label, boolean primary) {
        Button b = new Button(this);
        b.setText(UiText.t(label));
        b.setTextSize(11);
        b.setAllCaps(false);
        b.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        b.setTextColor(primary ? BG : Color.WHITE);
        b.setMinWidth(0);
        b.setMinHeight(0);
        b.setPadding(dp(12), 0, dp(12), 0);
        b.setElevation(0);
        b.setStateListAnimator(null);
        GradientDrawable base = background(primary ? GOLD : Color.rgb(29, 35, 45), 14,
                primary ? 0 : CARD_LINE);
        b.setBackground(new android.graphics.drawable.RippleDrawable(
                ColorStateList.valueOf(primary ? 0x33000000 : 0x33E6AE43), base, null));
        b.setFocusable(true);
        return b;
    }

    private void showAbout() {
        startActivity(new Intent(this, AboutActivity.class));
    }

    private void showLauncher() {
        binding = true;
        if(hasStorage()&&prefs().getBoolean("lsfg27_pending",false)&&gamePid27()==0)applyLsfgPending27();
        SharedPreferences p = prefs();
        LinearLayout screen = new LinearLayout(this);
        screen.setOrientation(LinearLayout.HORIZONTAL);
        screen.setPadding(dp(18), dp(14), dp(18), dp(14));
        screen.setBackground(new GradientDrawable(GradientDrawable.Orientation.TL_BR,
                new int[]{Color.rgb(29, 33, 45), BG, Color.rgb(47, 22, 20)}));

        LinearLayout hero = new LinearLayout(this);
        hero.setOrientation(LinearLayout.VERTICAL);
        hero.setGravity(Gravity.TOP);
        hero.setPadding(dp(18), dp(18), dp(18), dp(16));
        TextView badge = text("CONTATG  /  ANDROID ARM64", 9, GOLD, true);
        badge.setLetterSpacing(.10f);
        hero.addView(badge);
        TextView title = text("DEF JAM", 36, Color.WHITE, true);
        title.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        title.setLetterSpacing(-0.02f);
        LinearLayout.LayoutParams tp = new LinearLayout.LayoutParams(-1, -2);
        tp.topMargin = dp(6);
        hero.addView(title, tp);
        TextView build = text("FIGHT FOR NY", 15, GOLD, true);
        build.setLetterSpacing(.12f);
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(-1, -2);
        bp.topMargin = dp(2);
        hero.addView(build, bp);
        TextView edition = text("Android Native Port", 11, MUTED, false);
        LinearLayout.LayoutParams ep = new LinearLayout.LayoutParams(-1, -2);
        ep.topMargin = dp(10);
        hero.addView(edition, ep);
        View accent = new View(this);
        accent.setBackgroundColor(GOLD);
        LinearLayout.LayoutParams line = new LinearLayout.LayoutParams(dp(38), dp(3));
        line.topMargin = dp(16);
        hero.addView(accent, line);

        File xbe = new File(root(), "game/default.xbe");
        TextView status = text(xbe.isFile() ? "●  Game found" : "●  default.xbe not found", 12,
                xbe.isFile() ? GREEN : RED, true);
        LinearLayout.LayoutParams stp = new LinearLayout.LayoutParams(-1, -2);
        stp.topMargin = dp(22);
        hero.addView(status, stp);
        TextView path = text(root().getAbsolutePath() + File.separator + "game", 10, MUTED, false);
        path.setMaxLines(2);
        path.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        hero.addView(path);

        // Keep both launch actions outside the scrolling information column.
        LinearLayout launchActions = new LinearLayout(this);
        launchActions.setOrientation(LinearLayout.VERTICAL);
        launchActions.setPadding(dp(14), dp(4), dp(14), dp(14));
        Button play = button(hasStorage() ? "PLAY" : "ALLOW FILE ACCESS", true);
        LinearLayout.LayoutParams pp = new LinearLayout.LayoutParams(-1, dp(52));
        pp.topMargin = dp(8);
        launchActions.addView(play, pp);
        play.setOnClickListener(v -> play());
        emulatorOptions36=button(lsfgText27("Emulator options","Opções do emulador","Opciones del emulador"),false);
        LinearLayout.LayoutParams emulatorParams=new LinearLayout.LayoutParams(-1,dp(42));emulatorParams.topMargin=dp(6);
        launchActions.addView(emulatorOptions36,emulatorParams);
        emulatorOptions36.setVisibility(savedEmulatorMode(p)?View.VISIBLE:View.GONE);
        emulatorOptions36.setOnClickListener(v->HakuXOptions36.show(this));
        Button importIso = button("Extract Xbox ISO", false);
        LinearLayout.LayoutParams ip = new LinearLayout.LayoutParams(-1, dp(48)); ip.topMargin = dp(8);
        launchActions.addView(importIso, ip);
        importIso.setOnClickListener(v -> {
            if (!hasStorage()) { askForStorage(); return; }
            saveUiPreferences();
            startActivity(new Intent(this, IsoImportActivity.class));
        });

        TextView hint = text("Tip: start at 1x on mobile. 2x renders four times as many pixels.", 10, MUTED, false);
        LinearLayout.LayoutParams hp = new LinearLayout.LayoutParams(-1, -2);
        hp.topMargin = dp(10);
        hero.addView(hint, hp);

        LinearLayout right = new LinearLayout(this);
        right.setOrientation(LinearLayout.VERTICAL);
        right.addView(text("READY TO FIGHT", 25, Color.WHITE, true));
        right.addView(text(lsfgText27("Choose how to run the game and configure frame generation.","Escolha como executar o jogo e configure a geração de quadros.","Elige cómo ejecutar el juego y configurar la generación de fotogramas."),11,MUTED,false));

        language=spinner("English","Português (Brasil)","Español");
        language.setSelection(UiText.languageIndex());
        LinearLayout general = card("Preferences", "Graphics: original game Options > Graphics.");
        general.addView(row("Language", language));
        addCard(right, general);
        LinearLayout modes34=card(lsfgText27("Execution mode","Modo de execução","Modo de ejecución"),lsfgText27("Choose the native port or the built-in hakuX emulator.","Escolha o port nativo ou o emulador hakuX integrado.","Elige el port nativo o el emulador hakuX integrado."));
        execution34=spinner(lsfgText27("Native Vulkan","Nativo Vulkan","Nativo Vulkan"),lsfgText27("hakuX · emulator","hakuX · emulador","hakuX · emulador"));
        execution34.setSelection(savedEmulatorMode(p)?1:0);
        modes34.addView(row(lsfgText27("Mode","Modo","Modo"),execution34));
        addCard(right,modes34);
        addFramegenCard29(right);
        LinearLayout languages37=card(lsfgText27("Story subtitles and mods","Legendas da História e mods","Subtítulos de Historia y mods"),lsfgText27("Choose subtitles and translator-made language packs.","Escolha as legendas e pacotes de idioma de tradutores.","Elige subtítulos y paquetes de idioma de traductores."));
        Button manage37=button(lsfgText27("Manage subtitles and mods","Gerenciar legendas e mods","Gestionar subtítulos y mods"),false);
        languages37.addView(manage37);manage37.setOnClickListener(v->{if(!hasStorage()){askForStorage();return;}Subtitles37.show(this,root(),()->gamePid27()==0);});addCard(right,languages37);

        LinearLayout tools = card("Quick access", "Everything you need before entering the game.");
        Button compatibility136 = button("Fix Mali / PowerVR", false);
        Button account133 = button(UiText.tr("FFNY Online / Discord", "FFNY Online / Discord", "FFNY Online / Discord"), false);
        Button reports35 = button(lsfgText27("Error reports","Relatórios de erro","Informes de error"), false);
        Button about = button("About / Updates", false);
        Button[] toolButtons = {compatibility136, account133, reports35, about};
        for (Button tool : toolButtons) {
            tool.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
            tool.setPadding(dp(16), 0, dp(16), 0);
            LinearLayout.LayoutParams item = new LinearLayout.LayoutParams(-1, dp(42));
            item.topMargin = dp(5);
            tools.addView(tool, item);
        }
        compatibility136.setOnClickListener(v -> GpuCompatibility136.show(this));
        account133.setOnClickListener(v -> OnlineAccount133.show(this));
        reports35.setOnClickListener(v -> CrashReport35.show(this));
        about.setOnClickListener(v -> showAbout());
        addCard(right, tools);

        boolean installed = installedDriverValid();
        String installedName = p.getString("driver_name", "Imported Turnip");
        LinearLayout gpu = card("Vulkan driver", "Optional Turnip driver for Snapdragon/Adreno devices");
        driver = installed
                ? spinner("System • recommended", "Turnip • " + installedName)
                : spinner("System • recommended");
        boolean custom = installed && p.getBoolean("use_custom_driver", false);
        driver.setSelection(custom ? 1 : 0);
        gpu.addView(row("Active driver", driver));
        TextView driverStatus = text(installed
                        ? "● Installed package: " + p.getString("driver_library", "")
                        : "No Turnip package installed", 10, installed ? GREEN : MUTED, false);
        LinearLayout.LayoutParams dsp = new LinearLayout.LayoutParams(-1, -2);
        dsp.topMargin = dp(2);
        gpu.addView(driverStatus, dsp);
        LinearLayout driverButtons = new LinearLayout(this);
        Button importDriver = button(installed ? "Replace ZIP" : "Import Turnip (.zip)", false);
        Button removeDriver = button("Remove", false);
        removeDriver.setEnabled(installed);
        removeDriver.setAlpha(installed ? 1f : .4f);
        LinearLayout.LayoutParams ibp = new LinearLayout.LayoutParams(0, dp(40), 1);
        LinearLayout.LayoutParams rbp = new LinearLayout.LayoutParams(0, dp(40), .55f);
        rbp.leftMargin = dp(7);
        driverButtons.addView(importDriver, ibp);
        driverButtons.addView(removeDriver, rbp);
        LinearLayout.LayoutParams dbp = new LinearLayout.LayoutParams(-1, -2);
        dbp.topMargin = dp(8);
        gpu.addView(driverButtons, dbp);
        importDriver.setOnClickListener(v -> chooseDriverZip());
        removeDriver.setOnClickListener(v -> removeDriver());

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        addCard(content, gpu);
        right.addView(content);

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setOverScrollMode(View.OVER_SCROLL_NEVER);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.addView(right);
        ScrollView heroScroll = new ScrollView(this);
        heroScroll.setOverScrollMode(View.OVER_SCROLL_NEVER);
        heroScroll.setVerticalScrollBarEnabled(false);
        heroScroll.addView(hero);
        LinearLayout navigation = new LinearLayout(this);
        navigation.setOrientation(LinearLayout.VERTICAL);
        navigation.setBackground(background(Color.rgb(17, 21, 28), 22, CARD_LINE));
        navigation.addView(heroScroll, new LinearLayout.LayoutParams(-1, 0, 1));
        navigation.addView(launchActions, new LinearLayout.LayoutParams(-1, -2));
        screen.addView(navigation, new LinearLayout.LayoutParams(0, -1, .36f));
        LinearLayout.LayoutParams sr = new LinearLayout.LayoutParams(0, -1, .64f);
        sr.leftMargin = dp(20);
        screen.addView(scroll, sr);
        setContentView(screen);

        binding = false;
        bindPersistence();
    }

    private void addCard(LinearLayout parent, View card) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, -2);
        p.topMargin = dp(10);
        parent.addView(card, p);
    }

    private static String lsfgText27(String en,String pt,String es) {
        return UiText.isPortuguese()?pt:UiText.isSpanish()?es:en;
    }
    private void addFramegenCard29(LinearLayout parent) {
        LinearLayout c=card(lsfgText27("Frame generation","Geração de quadros","Generación de cuadros"),"Win-FG v37 / Lossless Scaling");
        Button open=button(lsfgText27("Configure frame generation","Configurar geração de quadros","Configurar generación de cuadros"),false);
        c.addView(open);open.setOnClickListener(v->{if(!hasStorage()){askForStorage();return;}startActivity(new Intent(this,FramegenActivity29.class));});addCard(parent,c);
    }

    /** 0=stopped, positive=our game PID, -1=unknown; never target another UID. */
    private int gamePid27() {
        try {
            android.app.ActivityManager manager=(android.app.ActivityManager)getSystemService(ACTIVITY_SERVICE);
            if(manager==null)return -1;
            List<android.app.ActivityManager.RunningAppProcessInfo> processes=manager.getRunningAppProcesses();
            if(processes==null)return -1;
            for(android.app.ActivityManager.RunningAppProcessInfo process:processes)
                if(process.uid==android.os.Process.myUid()&&process.pid!=android.os.Process.myPid()
                    &&(getPackageName()+":game").equals(process.processName))return process.pid;
            return 0;
        }catch(RuntimeException unavailable){return -1;}
    }
    private boolean applyLsfgPending27() {
        if(!prefs().getBoolean("lsfg27_pending",false))return true;
        int pid=gamePid27();
        if(pid>0)return true; // Resume existing match with its original settings; retain pending changes.
        if(pid<0){lsfgFailure27(lsfgText27("Close the game before applying","Feche o jogo antes de aplicar","Cierra el juego antes de aplicar"));return false;}
        try {
            LsfgSettings27.save(root(),new LsfgSettings27.State(prefs().getInt("fg_engine39_pending",0),prefs().getInt("lsfg27_flow",25),prefs().getInt("lsfg30_multiplier",2),prefs().getInt("lsfg30_refresh",60)),prefs().getBoolean("lsfg27_clear_effects",false));
            if(!prefs().edit().remove("lsfg27_pending").remove("lsfg27_clear_effects").commit())throw new java.io.IOException("Could not confirm launcher preferences");
            return true;
        }catch(Exception error){lsfgFailure27(error.getMessage());return false;}
    }
    private void showLsfgRestart27(boolean online,boolean playAfter) {
        new AlertDialog.Builder(this).setTitle(lsfgText27("Restart the game?","Reiniciar o jogo?","¿Reiniciar el juego?"))
            .setMessage(lsfgText27(
                "Frame generation settings need a new game session. Restarting ends the current match.",
                "A geração de quadros precisa de uma nova sessão. Reiniciar encerra a partida atual.",
                "La generación de fotogramas necesita una nueva sesión. Reiniciar termina la partida actual."))
            .setPositiveButton(lsfgText27("Restart now","Reiniciar agora","Reiniciar ahora"),(d,w)->{
                int pid=gamePid27();if(pid<0){lsfgFailure27("Close the game and try again");return;}
                if(pid>0)android.os.Process.killProcess(pid);
                waitForLsfgRestart27(online,true,0);
            }).setNegativeButton(lsfgText27("Later","Depois","Después"),(d,w)->{if(playAfter)launchConfigured27(online);}).show();
    }
    private void waitForLsfgRestart27(boolean online,boolean playAfter,int attempts) {
        if(isFinishing())return;
        if(gamePid27()==0) {
            if(applyLsfgPending27()){showLauncher();if(playAfter)play(online);}
            return;
        }
        if(attempts>=30){lsfgFailure27("Close the game and try again");return;}
        new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(()->waitForLsfgRestart27(online,playAfter,attempts+1),50);
    }
    private void lsfgFailure27(String detail) {
        Toast.makeText(this,lsfgText27("Could not apply framegen: ","Não foi possível aplicar a geração de quadros: ","No se pudo aplicar la generación: ")+detail,Toast.LENGTH_LONG).show();
    }

    private static boolean savedEmulatorMode(SharedPreferences prefs) {
        // Keep persisted IDs: 2 is hakuX; the old OpenGL ID 1 falls back to Vulkan.
        return prefs.getInt("execution34",0)==2;
    }
    private boolean emulatorSelected() {
        return execution34!=null&&execution34.getSelectedItemPosition()==1;
    }
    private void bindPersistence() {
        execution34.setOnSelectionChanged(()->{
            boolean emulated=emulatorSelected();
            prefs().edit().putInt("execution34",emulated?2:0).apply();
            emulatorOptions36.setVisibility(emulated?View.VISIBLE:View.GONE);
        });
        driver.setOnSelectionChanged(this::saveUiPreferences);
        language.setOnSelectionChanged(() -> {
            saveUiPreferences();UiText.setLanguage(UiText.languageCode(language.getSelectedItemPosition()));showLauncher();
        });
    }
    private void saveUiPreferences() {
        if(binding||driver==null||language==null)return;
        boolean custom=driver.getSelectedItemPosition()==1&&installedDriverValid();
        prefs().edit().putString("ui_language",UiText.languageCode(language.getSelectedItemPosition()))
            .putBoolean("use_custom_driver",custom).commit();
    }

    private void play() { play(false); }
    private void play(boolean online) {
        boolean emulated=!online&&emulatorSelected();
        if(!emulated&&HakuxLauncher34.running(this)){
            Toast.makeText(this,lsfgText27("Close the emulated game first.","Feche a emulação aberta primeiro.","Cierra primero el juego emulado."),Toast.LENGTH_LONG).show();return;
        }
        if(emulated){
            if(gamePid27()!=0){Toast.makeText(this,lsfgText27("Close the native game before opening hakuX.","Feche o jogo nativo antes de abrir o hakuX.","Cierra el juego nativo antes de abrir hakuX."),Toast.LENGTH_LONG).show();return;}
            if(!hasStorage()){askForStorage();return;}
            HakuxLauncher34.open(this);return;
        }
        if (!hasStorage()) {
            askForStorage();
            return;
        }
        File game = new File(root(), "game");
        game.mkdirs();
        if (!new File(game, "default.xbe").isFile()) {
            Toast.makeText(this, UiText.t("default.xbe not found in ") + game, Toast.LENGTH_LONG).show();
            return;
        }
        if(prefs().getBoolean("lsfg27_pending",false)&&gamePid27()!=0) {
            showLsfgRestart27(online,true);return;
        }
        launchConfigured27(online);
    }
    private void launchConfigured27(boolean online) {
        if(prefs().getBoolean("lsfg27_pending",false)&&!applyLsfgPending27())return;
        saveUiPreferences();
        try {
            saveIni();
        } catch (Exception e) {
            Toast.makeText(this, UiText.t("Could not save settings.ini: ") + e.getMessage(), Toast.LENGTH_LONG).show();
            return;
        }
        Bundle launch = new Bundle();
        SharedPreferences saved = prefs();
        launch.putInt("guest_cores", 1);
        launch.putInt("native_cores", 0);
        launch.putBoolean("split_native_render", true);
        launch.putBoolean("fxaa", saved.getBoolean("fxaa", false));
        launch.putString("ui_language", saved.getString("ui_language", "en"));
        launch.putBoolean("async_render", true);
        launch.putInt("gpu_compat136",GpuCompatibility136.read(this));
        launch.putBoolean("parallel_vertices", false);
        for (String key : new String[] {"touch_controls", "show_fps", "use_custom_driver"})
            launch.putBoolean(key, saved.getBoolean(key, !key.equals("use_custom_driver")));
        launch.putString("driver_library", saved.getString("driver_library", ""));
        Intent intent=new Intent(this,DefJamActivity.class).putExtra("defjam_launch",launch).putExtra("open_online",online);
        if(gamePid27()>0)intent.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT|Intent.FLAG_ACTIVITY_SINGLE_TOP);
        startActivity(intent);
    }

    /* Match the existing native precedence when loading the selector. On Play
     * the visible choice becomes authoritative for this one performance key. */
    private int readFpsIni139(int fallback) {
        try {
            File file=new File(root(),"settings.ini");
            if(!file.isFile())return fallback;
            return parseFpsIni139(Files.readAllLines(file.toPath(),StandardCharsets.UTF_8),fallback);
        } catch(Exception unavailable) { return fallback; }
    }
    static int parseFpsIni139(List<String> lines,int fallback) {
        String section="";int selected=fallback;
        for(String line:lines){
            String text=line.trim();int comment=text.indexOf(';');if(comment>=0)text=text.substring(0,comment).trim();
            comment=text.indexOf('#');if(comment>=0)text=text.substring(0,comment).trim();
            if(text.startsWith("[")&&text.endsWith("]")){section=text.substring(1,text.length()-1).trim();continue;}
            if(!section.equalsIgnoreCase("vksched"))continue;
            int eq=text.indexOf('=');if(eq<=0||!text.substring(0,eq).trim().equalsIgnoreCase("present_limit"))continue;
            try{int fps=Integer.parseInt(text.substring(eq+1).trim());
                if(fps==0||fps==30||fps==45||fps==60||fps==120)selected=fps;
                else if(fps==-1)selected=fallback;
            }catch(NumberFormatException invalid){ }
        }
        return selected;
    }

    private int readGraphics144(String key,int fallback,int lo,int hi) {
        try {
            File f=new File(root(),"settings.ini");if(!f.isFile())return fallback;
            String section="";int result=fallback;
            for(String line:Files.readAllLines(f.toPath(),StandardCharsets.UTF_8)){
                String text=line.trim();int c=text.indexOf(';');if(c>=0)text=text.substring(0,c).trim();
                c=text.indexOf('#');if(c>=0)text=text.substring(0,c).trim();
                if(text.startsWith("[")&&text.endsWith("]")){section=text.substring(1,text.length()-1).trim();continue;}
                if(!section.equalsIgnoreCase("graphics"))continue;
                int eq=text.indexOf('=');if(eq<=0||!text.substring(0,eq).trim().equalsIgnoreCase(key))continue;
                try{int n=Integer.parseInt(text.substring(eq+1).trim());if(n>=lo&&n<=hi)result=n;}catch(NumberFormatException ignored){}
            }return Math.max(lo,Math.min(hi,result));
        }catch(Exception ignored){return Math.max(lo,Math.min(hi,fallback));}
    }

    private void saveIni() throws Exception {
        if(!root().isDirectory()&&!root().mkdirs())throw new java.io.IOException("Could not create the data folder.");
        // Existing settings.ini is owned by the in-game graphics menu. Keep it byte-for-byte here.
    }

    private static void appendPending(List<String> out, String section,
                                      Map<String, Map<String, String>> pending) {
        Map<String, String> values = pending.get(section);
        if (values == null) return;
        for (Map.Entry<String, String> e : values.entrySet())
            out.add(e.getKey() + " = " + e.getValue());
        values.clear();
    }

    private void chooseDriverZip() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/zip");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
                "application/zip", "application/x-zip-compressed", "application/octet-stream"
        });
        startActivityForResult(i, DRIVER_REQUEST);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if(requestCode==Subtitles37.REQUEST){if(resultCode==RESULT_OK)Subtitles37.picked(this,data,root(),()->gamePid27()==0);return;}
        if (requestCode != DRIVER_REQUEST || resultCode != RESULT_OK || data == null || data.getData() == null)
            return;
        Uri uri = data.getData();
        Toast.makeText(this, UiText.t("Validating Turnip package…"), Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            try {
                DriverInfo info = installDriver(uri);
                prefs().edit()
                        .putString("driver_library", info.library)
                        .putString("driver_name", info.name)
                        .putBoolean("use_custom_driver", true)
                        .commit();
                runOnUiThread(() -> {
                    showLauncher();
                    Toast.makeText(this, UiText.t("Turnip installed. If loading fails, the app falls back to the system driver."),
                            Toast.LENGTH_LONG).show();
                });
            } catch (Exception e) {
                runOnUiThread(() -> Toast.makeText(this,
                        UiText.t("Invalid Turnip package: ") + e.getMessage(), Toast.LENGTH_LONG).show());
            }
        }, "TurnipImport").start();
    }

    private DriverInfo installDriver(Uri uri) throws Exception {
        File base = new File(getFilesDir(), "gpu_driver");
        if (!base.exists() && !base.mkdirs()) throw new Exception("could not create the private directory");
        File stage = new File(base, "incoming-" + System.nanoTime());
        if (!stage.mkdirs()) throw new Exception("could not prepare import");
        long total = 0;
        int entries = 0;
        InputStream source = getContentResolver().openInputStream(uri);
        if (source == null) throw new Exception("could not open file");
        try (InputStream input = source; ZipInputStream zip = new ZipInputStream(input)) {
            ZipEntry entry;
            byte[] buffer = new byte[64 * 1024];
            while ((entry = zip.getNextEntry()) != null) {
                if (++entries > MAX_DRIVER_ENTRIES) throw new Exception("too many files in ZIP");
                if (entry.isDirectory()) continue;
                String leaf = new File(entry.getName().replace('\\', '/')).getName();
                String lower = leaf.toLowerCase(Locale.ROOT);
                if (leaf.isEmpty() || !(lower.endsWith(".so") || lower.equals("meta.json"))) continue;
                File out = new File(stage, leaf);
                if (out.exists()) throw new Exception("duplicate filename in ZIP: " + leaf);
                try (FileOutputStream stream = new FileOutputStream(out)) {
                    int n;
                    while ((n = zip.read(buffer)) != -1) {
                        total += n;
                        if (total > MAX_DRIVER_BYTES) throw new Exception("unpacked ZIP exceeds 512 MB");
                        stream.write(buffer, 0, n);
                    }
                }
                out.setReadable(true, true);
                if (lower.endsWith(".so")) out.setExecutable(true, true);
            }
        } catch (Exception e) {
            deleteTree(stage);
            throw e;
        }

        JSONObject meta = null;
        File metaFile = new File(stage, "meta.json");
        if (!metaFile.isFile()) {
            File[] staged = stage.listFiles();
            if (staged != null) for (File file : staged)
                if (file.getName().equalsIgnoreCase("meta.json")) metaFile = file;
        }
        if (metaFile.isFile()) {
            try {
                meta = new JSONObject(new String(Files.readAllBytes(metaFile.toPath()), StandardCharsets.UTF_8));
            } catch (Exception ignored) {
                deleteTree(stage);
                throw new Exception("invalid meta.json");
            }
        }
        String requested = meta == null ? "" : new File(meta.optString("libraryName", "")).getName();
        File library = requested.isEmpty() ? null : new File(stage, requested);
        if (library == null || !library.isFile()) library = findDriverLibrary(stage);
        if (library == null) {
            deleteTree(stage);
            throw new Exception("Vulkan library not found");
        }
        if (!isArm64Elf(library)) {
            deleteTree(stage);
            throw new Exception("driver is not Android ARM64");
        }
        int minApi = meta == null ? 0 : meta.optInt("minApi", 0);
        if (minApi > Build.VERSION.SDK_INT) {
            deleteTree(stage);
            throw new Exception("driver requires Android API " + minApi);
        }
        String name = meta == null ? "Imported Turnip" : meta.optString("name", "Imported Turnip");
        String version = meta == null ? "" : meta.optString("driverVersion",
                meta.optString("packageVersion", ""));
        if (!version.isEmpty()) name = name + " " + version;
        if (name.length() > 46) name = name.substring(0, 46);

        File current = driverDir();
        File backup = new File(base, "previous");
        deleteTree(backup);
        if (current.exists() && !current.renameTo(backup)) {
            deleteTree(stage);
            throw new Exception("could not replace the previous driver");
        }
        if (!stage.renameTo(current)) {
            if (backup.exists()) backup.renameTo(current);
            deleteTree(stage);
            throw new Exception("could not activate the package");
        }
        deleteTree(backup);
        return new DriverInfo(library.getName(), name);
    }

    private File findDriverLibrary(File dir) {
        String[] preferred = {"libvulkan_freedreno.so", "libvulkan_turnip.so", "vulkan.ad07xx.so"};
        for (String name : preferred) {
            File f = new File(dir, name);
            if (f.isFile()) return f;
        }
        File[] files = dir.listFiles();
        if (files == null) return null;
        for (File f : files) {
            String n = f.getName().toLowerCase(Locale.ROOT);
            if (f.isFile() && n.endsWith(".so") && n.contains("vulkan")) return f;
        }
        return null;
    }

    private boolean isArm64Elf(File file) {
        byte[] h = new byte[20];
        try (FileInputStream in = new FileInputStream(file)) {
            if (in.read(h) != h.length) return false;
            return h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F'
                    && h[4] == 2 && h[5] == 1
                    && (h[18] & 0xff) == 0xb7 && h[19] == 0;
        } catch (Exception e) {
            return false;
        }
    }

    private boolean installedDriverValid() {
        String library = prefs().getString("driver_library", "");
        return !library.isEmpty() && new File(driverDir(), library).isFile();
    }

    private void removeDriver() {
        deleteTree(new File(getFilesDir(), "gpu_driver"));
        prefs().edit().remove("driver_library").remove("driver_name")
                .putBoolean("use_custom_driver", false).commit();
        showLauncher();
        Toast.makeText(this, UiText.t("Driver removed. System Vulkan selected."), Toast.LENGTH_SHORT).show();
    }

    private static void deleteTree(File file) {
        if (file == null || !file.exists()) return;
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        file.delete();
    }

    private void askForStorage() {
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else {
            requestPermissions(new String[] {
                    Manifest.permission.READ_EXTERNAL_STORAGE,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE
            }, STORAGE_REQUEST);
        }
    }

    private static final class DriverInfo {
        final String library;
        final String name;
        DriverInfo(String library, String name) {
            this.library = library;
            this.name = name;
        }
    }
}
