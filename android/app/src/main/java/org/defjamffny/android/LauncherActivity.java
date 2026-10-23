package org.defjamffny.android;

import android.Manifest;
import android.app.Activity;
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
    private static final int BG = Color.rgb(9, 10, 12);
    private static final int CARD = Color.rgb(20, 22, 26);
    private static final int CARD_LINE = Color.rgb(43, 46, 53);
    private static final int GOLD = Color.rgb(230, 174, 67);
    private static final int MUTED = Color.rgb(166, 170, 178);
    private static final int GREEN = Color.rgb(102, 211, 145);
    private static final int RED = Color.rgb(238, 111, 103);

    private ChoicePill scale;
    private ChoicePill aspect;
    private ChoicePill controlLayout;
    private ChoicePill driver;
    private TogglePill vsync;
    private TogglePill smooth;
    private TogglePill postEffects;
    private TogglePill hdTextures;
    private TogglePill showFps;
    private TogglePill parallelVertices;
    private TogglePill asyncRender;
    private TogglePill touchControls;
    private ValueSlider controlSize;
    private ValueSlider controlOpacity;
    private ValueSlider deadzone;
    private ValueSlider rumble;
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
        migrateFromBrokenLayout();
        if (!prefs().getBoolean("v13_recovery", false)) {
            prefs().edit().putBoolean("framegen", false).putBoolean("parallel_vertices", false)
                    .putBoolean("v13_recovery", true).commit();
        }
        if (!prefs().getBoolean("v18_native_parallel", false)) {
            // v13 recovery disabled this option. The new native budget has
            // its own scope and does not widen guest affinity.
            prefs().edit().putBoolean("parallel_vertices", true).putInt("native_cores", 0)
                    .putBoolean("v18_native_parallel", true).commit();
        }
        showLauncher();
    }

    @Override
    protected void onResume() {
        super.onResume();
        forceLandscape();
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
        t.setText(value);
        t.setTextSize(size);
        t.setTextColor(color);
        t.setGravity(Gravity.CENTER_VERTICAL);
        if (bold) t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return t;
    }

    private LinearLayout card(String title, String subtitle) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(16), dp(13), dp(16), dp(13));
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
            this.values = values;
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
            setText(value ? "LIGADO" : "DESLIGADO");
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
        b.setText(label);
        b.setTextSize(11);
        b.setAllCaps(false);
        b.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        b.setTextColor(primary ? BG : Color.WHITE);
        b.setMinWidth(0);
        b.setMinHeight(0);
        b.setPadding(dp(12), 0, dp(12), 0);
        b.setElevation(0);
        b.setStateListAnimator(null);
        b.setBackground(background(primary ? GOLD : Color.rgb(31, 34, 40), 13,
                primary ? 0 : Color.rgb(58, 62, 70)));
        return b;
    }

    private void showLauncher() {
        binding = true;
        SharedPreferences p = prefs();
        LinearLayout screen = new LinearLayout(this);
        screen.setOrientation(LinearLayout.HORIZONTAL);
        screen.setPadding(dp(20), dp(16), dp(20), dp(16));
        screen.setBackgroundColor(BG);

        LinearLayout hero = new LinearLayout(this);
        hero.setOrientation(LinearLayout.VERTICAL);
        hero.setGravity(Gravity.CENTER_VERTICAL);
        hero.setPadding(dp(10), 0, dp(22), 0);
        hero.addView(text("NEW YORK  /  ANDROID ARM64", 10, GOLD, true));
        TextView title = text("DEF JAM\nFIGHT FOR NY", 31, Color.WHITE, true);
        title.setLetterSpacing(-0.02f);
        LinearLayout.LayoutParams tp = new LinearLayout.LayoutParams(-1, -2);
        tp.topMargin = dp(6);
        hero.addView(title, tp);
        TextView build = text("Recompilação nativa • configuração local", 12, MUTED, false);
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(-1, -2);
        bp.topMargin = dp(7);
        hero.addView(build, bp);

        File xbe = new File(root(), "game/default.xbe");
        TextView status = text(xbe.isFile() ? "●  Jogo encontrado" : "●  default.xbe não encontrado", 12,
                xbe.isFile() ? GREEN : RED, true);
        LinearLayout.LayoutParams stp = new LinearLayout.LayoutParams(-1, -2);
        stp.topMargin = dp(22);
        hero.addView(status, stp);
        hero.addView(text(root().getAbsolutePath() + File.separator + "game", 10, MUTED, false));

        Button play = button(hasStorage() ? "JOGAR" : "LIBERAR ARQUIVOS", true);
        LinearLayout.LayoutParams pp = new LinearLayout.LayoutParams(-1, dp(52));
        pp.topMargin = dp(20);
        hero.addView(play, pp);
        play.setOnClickListener(v -> play());

        TextView hint = text("Dica: em celular, comece em 1x. 2x usa quatro vezes mais pixels.", 10, MUTED, false);
        LinearLayout.LayoutParams hp = new LinearLayout.LayoutParams(-1, -2);
        hp.topMargin = dp(10);
        hero.addView(hint, hp);

        LinearLayout right = new LinearLayout(this);
        right.setOrientation(LinearLayout.VERTICAL);
        right.addView(text("Configurações", 22, Color.WHITE, true));
        right.addView(text("As alterações são salvas na hora.", 11, MUTED, false));

        LinearLayout perf = card("Desempenho", "1x é o perfil recomendado para manter FPS no celular");
        LinearLayout presets = new LinearLayout(this);
        presets.setGravity(Gravity.CENTER);
        String[] names = {"Economia", "Equilibrado", "Qualidade"};
        for (int i = 0; i < names.length; i++) {
            final int preset = i;
            Button b = button(names[i], false);
            LinearLayout.LayoutParams q = new LinearLayout.LayoutParams(0, dp(40), 1);
            if (i > 0) q.leftMargin = dp(7);
            presets.addView(b, q);
            b.setOnClickListener(v -> applyPreset(preset));
        }
        LinearLayout.LayoutParams prp = new LinearLayout.LayoutParams(-1, -2);
        prp.topMargin = dp(7);
        perf.addView(presets, prp);

        scale = spinner("1x • 480p", "2x • 960p", "3x • 1440p", "4x • 1920p");
        scale.setSelection(Math.max(0, Math.min(3, p.getInt("render_scale", 1) - 1)));
        aspect = spinner("4:3 original", "Esticar", "Widescreen automático", "16:9", "Tela cheia (aparelho)", "21:9 (experimental)"); // DEFJAM_V5
        aspect.setSelection(Math.max(0, Math.min(5, p.getInt("aspect", 2))));
        vsync = toggle(p.getBoolean("vsync", false));
        smooth = toggle(p.getBoolean("smooth", true));
        postEffects = toggle(p.getBoolean("post_effects", true));
        hdTextures = toggle(p.getBoolean("hd_textures", false));
        showFps = toggle(p.getBoolean("show_fps", true));
        parallelVertices = toggle(p.getBoolean("parallel_vertices", true));
        asyncRender = toggle(p.getBoolean("async_render", true));
        perf.addView(row("Resolução interna", scale));
        perf.addView(row("Formato da imagem", aspect));
        perf.addView(row("VSync", vsync));
        perf.addView(row("Filtro suave", smooth));
        perf.addView(row("Gamma e vinheta", postEffects));
        perf.addView(row("Texturas HD/mods", hdTextures));
        perf.addView(row("Mostrar FPS", showFps));
        perf.addView(row("Preparar vértices em paralelo • experimental", parallelVertices));
        perf.addView(row("Renderização assíncrona • reiniciar jogo", asyncRender));

        boolean installed = installedDriverValid();
        String installedName = p.getString("driver_name", "Turnip importado");
        LinearLayout gpu = card("Driver Vulkan", "Turnip é opcional e indicado apenas para Snapdragon/Adreno");
        driver = installed
                ? spinner("Sistema • recomendado", "Turnip • " + installedName)
                : spinner("Sistema • recomendado");
        boolean custom = installed && p.getBoolean("use_custom_driver", false);
        driver.setSelection(custom ? 1 : 0);
        gpu.addView(row("Driver ativo", driver));
        TextView driverStatus = text(installed
                        ? "● Pacote instalado: " + p.getString("driver_library", "")
                        : "Nenhum pacote Turnip instalado", 10, installed ? GREEN : MUTED, false);
        LinearLayout.LayoutParams dsp = new LinearLayout.LayoutParams(-1, -2);
        dsp.topMargin = dp(2);
        gpu.addView(driverStatus, dsp);
        LinearLayout driverButtons = new LinearLayout(this);
        Button importDriver = button(installed ? "Trocar ZIP" : "Importar Turnip (.zip)", false);
        Button removeDriver = button("Remover", false);
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

        LinearLayout controls = card("Controles na tela", "Tamanho limitado à área útil para não cortar nem sobrepor");
        touchControls = toggle(p.getBoolean("touch_controls", true));
        controlLayout = spinner("Clássico", "Compacto", "Canhoto");
        controlLayout.setSelection(Math.max(0, Math.min(2, p.getInt("control_layout", 1))));
        controlSize = seek(50, p.getInt("control_size", 90) - 70, 70);
        controlOpacity = seek(70, p.getInt("control_opacity", 58) - 20, 20);
        deadzone = seek(35, p.getInt("deadzone", 15), 0);
        rumble = seek(100, p.getInt("rumble", 75), 0);
        controls.addView(row("Ativar controles", touchControls));
        controls.addView(row("Layout", controlLayout));
        controls.addView(row("Tamanho (70–120%)", controlSize));
        controls.addView(row("Opacidade (20–90%)", controlOpacity));
        controls.addView(row("Deadzone", deadzone));
        controls.addView(row("Vibração", rumble));
        Button editControls = button("Editar controles na tela", true);
        LinearLayout.LayoutParams ecp = new LinearLayout.LayoutParams(-1, dp(40));
        ecp.topMargin = dp(7);
        controls.addView(editControls, ecp);
        editControls.setOnClickListener(v -> startActivity(new Intent(this, ControlsEditorActivity.class)));

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        addCard(content, perf);
        addCard(content, gpu);
        addCard(content, controls);
        right.addView(content);

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setOverScrollMode(View.OVER_SCROLL_NEVER);
        scroll.addView(right);
        screen.addView(hero, new LinearLayout.LayoutParams(0, -1, .36f));
        LinearLayout.LayoutParams sr = new LinearLayout.LayoutParams(0, -1, .64f);
        sr.leftMargin = dp(8);
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

    private void bindPersistence() {
        Runnable save = this::saveUiPreferences;
        scale.setOnSelectionChanged(save);
        aspect.setOnSelectionChanged(save);
        controlLayout.setOnSelectionChanged(save);
        driver.setOnSelectionChanged(save);
        vsync.setOnCheckedChanged(save);
        smooth.setOnCheckedChanged(save);
        postEffects.setOnCheckedChanged(save);
        hdTextures.setOnCheckedChanged(save);
        showFps.setOnCheckedChanged(save);
        parallelVertices.setOnCheckedChanged(save);
        asyncRender.setOnCheckedChanged(save);
        touchControls.setOnCheckedChanged(save);
        controlSize.setOnValueChanged(save);
        controlOpacity.setOnValueChanged(save);
        deadzone.setOnValueChanged(save);
        rumble.setOnValueChanged(save);
    }

    private void saveUiPreferences() {
        if (binding || scale == null || driver == null) return;
        boolean useCustom = driver.getSelectedItemPosition() == 1 && installedDriverValid();
        prefs().edit()
                .putInt("render_scale", scale.getSelectedItemPosition() + 1)
                .putInt("aspect", aspect.getSelectedItemPosition())
                .putInt("guest_cores", 1)
                .putInt("native_cores", 0)
                .putBoolean("vsync", vsync.isChecked())
                .putBoolean("smooth", smooth.isChecked())
                .putBoolean("post_effects", postEffects.isChecked())
                .putBoolean("hd_textures", hdTextures.isChecked())
                .putBoolean("show_fps", showFps.isChecked())
                .putBoolean("parallel_vertices", parallelVertices.isChecked())
                .putBoolean("async_render", asyncRender.isChecked())
                .putBoolean("touch_controls", touchControls.isChecked())
                .putInt("control_layout", controlLayout.getSelectedItemPosition())
                .putInt("control_size", controlSize.getProgress() + 70)
                .putInt("control_opacity", controlOpacity.getProgress() + 20)
                .putInt("deadzone", deadzone.getProgress())
                .putInt("rumble", rumble.getProgress())
                .putBoolean("use_custom_driver", useCustom)
                .commit();
    }

    private void applyPreset(int preset) {
        if (preset == 0) {
            scale.setSelection(0);
            vsync.setChecked(false);
            smooth.setChecked(false);
            postEffects.setChecked(false);
            hdTextures.setChecked(false);
        } else if (preset == 1) {
            scale.setSelection(0);
            vsync.setChecked(false);
            smooth.setChecked(true);
            postEffects.setChecked(true);
            hdTextures.setChecked(false);
        } else {
            scale.setSelection(1);
            vsync.setChecked(true);
            smooth.setChecked(true);
            postEffects.setChecked(true);
        }
        saveUiPreferences();
        Toast.makeText(this, preset == 0 ? "Economia ativada"
                : preset == 1 ? "Equilibrado ativado" : "Qualidade ativada", Toast.LENGTH_SHORT).show();
    }

    private void play() {
        if (!hasStorage()) {
            askForStorage();
            return;
        }
        File game = new File(root(), "game");
        game.mkdirs();
        if (!new File(game, "default.xbe").isFile()) {
            Toast.makeText(this, "default.xbe não encontrado em " + game, Toast.LENGTH_LONG).show();
            return;
        }
        saveUiPreferences();
        try {
            saveIni();
        } catch (Exception e) {
            Toast.makeText(this, "Não consegui salvar settings.ini: " + e.getMessage(), Toast.LENGTH_LONG).show();
            return;
        }
        Bundle launch = new Bundle();
        SharedPreferences saved = prefs();
        launch.putInt("guest_cores", 1);
        launch.putInt("native_cores", 0);
        launch.putInt("fps_limit", saved.getInt("fps_limit", 60));
        for (String key : new String[] {"parallel_vertices", "async_render", "touch_controls", "show_fps", "use_custom_driver"})
            launch.putBoolean(key, saved.getBoolean(key, !key.equals("use_custom_driver")));
        launch.putString("driver_library", saved.getString("driver_library", ""));
        startActivity(new Intent(this, DefJamActivity.class).putExtra("defjam_launch", launch));
    }

    private void saveIni() throws Exception {
        File file = new File(root(), "settings.ini");
        Map<String, Map<String, String>> changes = new LinkedHashMap<>();
        String[] aspectValues = {"4:3", "stretch", "widescreen (auto)", "16:9", "widescreen (auto)", "21:9 (experimental)"};
        put(changes, "display", "render_scale", Integer.toString(scale.getSelectedItemPosition() + 1));
        put(changes, "display", "aspect", aspectValues[aspect.getSelectedItemPosition()]);
        put(changes, "display", "fullscreen", "1"); // DEFJAM_V6: borderless full screen, the window is the whole display
        {
            /* "Tela cheia": the widescreen auto shape is read from the window size, so
             * give it the device's real landscape size; every other choice gets 16:9. */
            int ww = 1920, hh = 1080;
            if (aspect.getSelectedItemPosition() == 4) {
                android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
                getWindowManager().getDefaultDisplay().getRealMetrics(dm);
                ww = Math.max(dm.widthPixels, dm.heightPixels);
                hh = Math.min(dm.widthPixels, dm.heightPixels);
            }
            put(changes, "display", "window_width", Integer.toString(ww));
            put(changes, "display", "window_height", Integer.toString(hh));
        }
        put(changes, "display", "filter", smooth.isChecked() ? "smooth" : "sharp");
        put(changes, "display", "vsync", vsync.isChecked() ? "1" : "0");
        put(changes, "display", "gamma", postEffects.isChecked() ? "1" : "0");
        put(changes, "display", "vignette", postEffects.isChecked() ? "1" : "0");
        put(changes, "textures", "enabled", hdTextures.isChecked() ? "1" : "0");
        put(changes, "input", "rumble", Integer.toString(rumble.getProgress()));
        put(changes, "gamepad", "left_deadzone", Integer.toString(deadzone.getProgress()));
        put(changes, "gamepad", "right_deadzone", Integer.toString(deadzone.getProgress()));

        List<String> old = file.isFile()
                ? Files.readAllLines(file.toPath(), StandardCharsets.UTF_8) : new ArrayList<>();
        List<String> out = new ArrayList<>();
        String section = "";
        Map<String, Map<String, String>> pending = new LinkedHashMap<>();
        for (Map.Entry<String, Map<String, String>> e : changes.entrySet())
            pending.put(e.getKey(), new LinkedHashMap<>(e.getValue()));
        for (String line : old) {
            String trim = line.trim();
            if (trim.startsWith("[") && trim.endsWith("]")) {
                appendPending(out, section, pending);
                section = trim.substring(1, trim.length() - 1).trim().toLowerCase(Locale.ROOT);
                out.add(line);
                continue;
            }
            int eq = trim.indexOf('=');
            if (eq > 0 && pending.containsKey(section)) {
                String key = trim.substring(0, eq).trim().toLowerCase(Locale.ROOT);
                String value = pending.get(section).remove(key);
                if (value != null) {
                    out.add(key + " = " + value);
                    continue;
                }
            }
            out.add(line);
        }
        appendPending(out, section, pending);
        for (String sec : new ArrayList<>(pending.keySet())) {
            if (pending.get(sec).isEmpty()) continue;
            if (!out.isEmpty() && !out.get(out.size() - 1).isEmpty()) out.add("");
            out.add("[" + sec + "]");
            appendPending(out, sec, pending);
        }
        root().mkdirs();
        Files.write(file.toPath(), out, StandardCharsets.UTF_8);
    }

    private static void put(Map<String, Map<String, String>> map, String section, String key, String value) {
        if (!map.containsKey(section)) map.put(section, new LinkedHashMap<>());
        map.get(section).put(key, value);
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
        if (requestCode != DRIVER_REQUEST || resultCode != RESULT_OK || data == null || data.getData() == null)
            return;
        Uri uri = data.getData();
        Toast.makeText(this, "Validando pacote Turnip…", Toast.LENGTH_SHORT).show();
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
                    Toast.makeText(this, "Turnip instalado. Se falhar, o app volta ao driver do sistema.",
                            Toast.LENGTH_LONG).show();
                });
            } catch (Exception e) {
                runOnUiThread(() -> Toast.makeText(this,
                        "Pacote Turnip inválido: " + e.getMessage(), Toast.LENGTH_LONG).show());
            }
        }, "TurnipImport").start();
    }

    private DriverInfo installDriver(Uri uri) throws Exception {
        File base = new File(getFilesDir(), "gpu_driver");
        if (!base.exists() && !base.mkdirs()) throw new Exception("não foi possível criar a pasta privada");
        File stage = new File(base, "incoming-" + System.nanoTime());
        if (!stage.mkdirs()) throw new Exception("não foi possível preparar a importação");
        long total = 0;
        int entries = 0;
        InputStream source = getContentResolver().openInputStream(uri);
        if (source == null) throw new Exception("arquivo não pôde ser aberto");
        try (InputStream input = source; ZipInputStream zip = new ZipInputStream(input)) {
            ZipEntry entry;
            byte[] buffer = new byte[64 * 1024];
            while ((entry = zip.getNextEntry()) != null) {
                if (++entries > MAX_DRIVER_ENTRIES) throw new Exception("ZIP possui arquivos demais");
                if (entry.isDirectory()) continue;
                String leaf = new File(entry.getName().replace('\\', '/')).getName();
                String lower = leaf.toLowerCase(Locale.ROOT);
                if (leaf.isEmpty() || !(lower.endsWith(".so") || lower.equals("meta.json"))) continue;
                File out = new File(stage, leaf);
                if (out.exists()) throw new Exception("nome duplicado no ZIP: " + leaf);
                try (FileOutputStream stream = new FileOutputStream(out)) {
                    int n;
                    while ((n = zip.read(buffer)) != -1) {
                        total += n;
                        if (total > MAX_DRIVER_BYTES) throw new Exception("ZIP descompactado excede 512 MB");
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
                throw new Exception("meta.json corrompido");
            }
        }
        String requested = meta == null ? "" : new File(meta.optString("libraryName", "")).getName();
        File library = requested.isEmpty() ? null : new File(stage, requested);
        if (library == null || !library.isFile()) library = findDriverLibrary(stage);
        if (library == null) {
            deleteTree(stage);
            throw new Exception("biblioteca Vulkan não encontrada");
        }
        if (!isArm64Elf(library)) {
            deleteTree(stage);
            throw new Exception("driver não é Android ARM64");
        }
        int minApi = meta == null ? 0 : meta.optInt("minApi", 0);
        if (minApi > Build.VERSION.SDK_INT) {
            deleteTree(stage);
            throw new Exception("driver exige Android API " + minApi);
        }
        String name = meta == null ? "Turnip importado" : meta.optString("name", "Turnip importado");
        String version = meta == null ? "" : meta.optString("driverVersion",
                meta.optString("packageVersion", ""));
        if (!version.isEmpty()) name = name + " " + version;
        if (name.length() > 46) name = name.substring(0, 46);

        File current = driverDir();
        File backup = new File(base, "previous");
        deleteTree(backup);
        if (current.exists() && !current.renameTo(backup)) {
            deleteTree(stage);
            throw new Exception("não foi possível substituir o driver anterior");
        }
        if (!stage.renameTo(current)) {
            if (backup.exists()) backup.renameTo(current);
            deleteTree(stage);
            throw new Exception("não foi possível ativar o pacote");
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
        Toast.makeText(this, "Driver removido. Vulkan do sistema selecionado.", Toast.LENGTH_SHORT).show();
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
