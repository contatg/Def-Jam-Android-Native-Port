package org.defjamffny.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.os.Build;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import android.util.AtomicFile;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * On-screen fight pad: two analog sticks (drawn), the artwork buttons and an
 * optional d-pad. Every control can be moved, resized, hidden and (for the
 * buttons) re-assigned in ControlsEditorActivity. Positions are fractions of the
 * view, so they survive any screen.
 */
public final class VirtualControlsView extends View {
    static final int N = 14;
    static final int STICK_L = 0, STICK_R = 1, SOCO = 2, CHUTE = 3, AGARRAR = 4, DEFENDER = 5,
            ESPECIAL = 6, CORRER = 7, PAUSA = 8, CONFIG = 9, UP = 10, DOWN = 11, LEFT = 12, RIGHT = 13;

    private static final String[] NAMES = {
        "Analógico esquerdo", "Analógico direito", "SOCO", "CHUTE", "AGARRAR", "DEFENDER",
        "ESPECIAL", "CORRER", "START / PAUSA", "AJUSTES", "D-PAD (junto)", "Baixo", "Esquerda", "Direita"
    };
    private static final String[] SPRITE = {
        null, null, "ctl_soco", "ctl_chute", "ctl_agarrar", "ctl_defender", "ctl_especial",
        "ctl_correr", "ctl_pause", "ctl_config", "ctl_up", "ctl_down", "ctl_left", "ctl_right"
    };
    /* Artwork geometry in the source picture's pixels: centre x, centre y, width, height of the
     * button itself, then the width and height of the drawn sprite (it includes the glow). */
    private static final float[][] ART = {
        {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0},
        {1511f, 694f, 290f, 288f, 297f, 293f},     // SOCO
        {1226f, 781.5f, 264f, 261f, 271f, 274f},   // CHUTE
        {1159f, 560.5f, 190f, 189f, 195f, 202f},   // AGARRAR
        {1344.5f, 482f, 187f, 188f, 195f, 192f},   // DEFENDER
        {1533.5f, 387.5f, 207f, 221f, 214f, 228f}, // ESPECIAL
        {996.5f, 706f, 173f, 174f, 180f, 179f},    // CORRER
        {755.5f, 54f, 89f, 88f, 94f, 94f},         // PAUSA
        {917.5f, 53.5f, 89f, 89f, 94f, 93f},       // AJUSTES
        {209f, 586.5f, 138f, 125f, 152f, 136f},    // cima
        {209f, 813.5f, 140f, 125f, 152f, 135f},    // baixo
        {90f, 699.5f, 134f, 129f, 144f, 145f},     // esquerda
        {328f, 699f, 134f, 130f, 145f, 145f}       // direita
    };
    /* What each assignable button can press (the keyboard keys of the port's pad). */
    static final String[] KEY_LABEL = {"X", "A", "Y", "B", "LB", "RB", "LT", "RT", "START", "BACK", "MENU"};
    private static final int[] KEY_CODE = {
        KeyEvent.KEYCODE_J, KeyEvent.KEYCODE_K, KeyEvent.KEYCODE_I, KeyEvent.KEYCODE_L,
        KeyEvent.KEYCODE_U, KeyEvent.KEYCODE_O, KeyEvent.KEYCODE_Q, KeyEvent.KEYCODE_E,
        KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_DEL, KeyEvent.KEYCODE_F1
    };
    // Confirmed in play: Y punches, X kicks, A grapples.
    private static final int[] DEFAULT_KEY = {-1, -1, 2, 0, 1, 7, 6, 3, 8, 10, -1, -1, -1, -1};
    private static final int[] DPAD_KEY = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        KeyEvent.KEYCODE_W, KeyEvent.KEYCODE_S,
        KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_D
    };

    private final DefJamActivity activity;
    private final boolean edit;
    private boolean menuOnly;
    private final Paint spritePaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint selPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path chevron = new Path();
    private final Bitmap[] bmp = new Bitmap[N];
    private final RectF dst = new RectF();
    private final Map<Integer, Set<Integer>> pointerKeys = new HashMap<>();
    private final Map<Integer, Set<Integer>> pointerCtls = new HashMap<>();
    private final Map<Integer, Integer> stickOwner = new HashMap<>();
    private final Set<Integer> held = new HashSet<>();
    private final boolean[] pressed = new boolean[N];
    private final float userScale;
    private final float deadzone;
    private final float spriteAlpha;
    private final int hapticAmplitude;
    private final Vibrator vibrator;
    private long lastHaptic;

    private float w, h, k;
    private final float[] cx = new float[N];
    private final float[] cy = new float[N];
    private final float[] hw = new float[N];     // half extent used for hit tests and the selection box
    private final float[] hh = new float[N];
    private final boolean[] hasPos = new boolean[N];
    private final float[] fx = new float[N];
    private final float[] fy = new float[N];
    private final float[] mult = new float[N];
    private final boolean[] visible = new boolean[N];
    private final int[] keyIdx = new int[N];
    private final float[] knobX = new float[2];
    private final float[] knobY = new float[2];
    private final float[] shaderR = new float[2];
    private final Shader[] baseShader = new Shader[2];
    private final Shader[] knobShader = new Shader[2];

    private final DpadLayout dpad = new DpadLayout();
    private int selected = -1;
    private float grabDx, grabDy;
    private boolean dragging;
    private boolean mappingMigrationPending;
    private Runnable onSelect;

    public VirtualControlsView(DefJamActivity context) {
        this(context, context, false);
    }

    public VirtualControlsView(Context context, DefJamActivity game, boolean editMode) {
        super(context);
        activity = game;
        edit = editMode;
        setFocusable(false);
        setWillNotDraw(false);
        SharedPreferences p = context.getSharedPreferences("launcher", Context.MODE_PRIVATE);
        userScale = clamp(p.getInt("control_size", 90) / 100f, .70f, 1.20f);
        deadzone = clamp(p.getInt("deadzone", 15) / 100f, .12f, .40f);
        spriteAlpha = clamp(p.getInt("control_opacity", 58) / 100f, .45f, 1f);
        int rumble = Math.max(0, Math.min(100, p.getInt("rumble", 75)));
        hapticAmplitude = rumble == 0 ? 0 : Math.max(18, Math.round(255f * rumble / 100f));
        vibrator = (Vibrator)context.getSystemService(Context.VIBRATOR_SERVICE);
        strokePaint.setStyle(Paint.Style.STROKE);
        selPaint.setStyle(Paint.Style.STROKE);
        selPaint.setStrokeWidth(dp(2.2f));
        selPaint.setColor(Color.WHITE);
        for (int i = SOCO; i < N; i++) bmp[i] = load(SPRITE[i]);
        // v7 default matches the requested artwork: arrows for movement, no sticks.
        // Existing positions, sizes and button bindings remain intact.
        JSONObject saved = readLayout();
        boolean migrate = saved == null || !saved.optBoolean("controls_v7_arrows", false);
        for (int i = 0; i < N; i++) {
            mult[i] = clamp((saved == null ? p.getInt("c6_" + i + "_s", 100)
                    : saved.optInt("c6_" + i + "_s", 100)) / 100f, .5f, 2.2f);
            visible[i] = saved == null ? p.getBoolean("c6_" + i + "_v", i != STICK_L && i != STICK_R)
                    : saved.optBoolean("c6_" + i + "_v", i != STICK_L && i != STICK_R);
            if (migrate && (isStick(i) || isDpad(i))) visible[i] = isDpad(i);
            keyIdx[i] = Math.max(-1, Math.min(KEY_CODE.length - 1, saved == null
                    ? p.getInt("c6_" + i + "_k", DEFAULT_KEY[i]) : saved.optInt("c6_" + i + "_k", DEFAULT_KEY[i])));
            boolean positioned = saved == null ? p.contains("c6_" + i + "_x") && p.contains("c6_" + i + "_y")
                    : saved.has("c6_" + i + "_x") && saved.has("c6_" + i + "_y");
            if (positioned) {
                hasPos[i] = true;
                fx[i] = clamp(saved == null ? p.getFloat("c6_" + i + "_x", .5f)
                        : (float)saved.optDouble("c6_" + i + "_x", .5), 0f, 1f);
                fy[i] = clamp(saved == null ? p.getFloat("c6_" + i + "_y", .5f)
                        : (float)saved.optDouble("c6_" + i + "_y", .5), 0f, 1f);
            }
        }
        if (saved != null && saved.optBoolean("dpad_v8", false)) {
            dpad.x = clamp((float)saved.optDouble("dpad_x", .15), 0, 1);
            dpad.y = clamp((float)saved.optDouble("dpad_y", .75), 0, 1);
            dpad.scale = clamp((float)saved.optDouble("dpad_scale", 1), .5f, 2.2f);
            dpad.visible = saved.optBoolean("dpad_visible", true);
            dpad.positioned = true;
        } else {
            // Import an old split layout as one group centred on the old arrows.
            float x = 0, y = 0, scale = 0;
            int count = 0; boolean anyVisible = false;
            for (int i = UP; i <= RIGHT; i++) {
                if (hasPos[i]) { x += fx[i]; y += fy[i]; count++; }
                scale += mult[i]; anyVisible |= visible[i];
            }
            if (count > 0) { dpad.x = x / count; dpad.y = y / count; dpad.positioned = true; }
            dpad.scale = scale / 4; dpad.visible = anyVisible;
        }
        if (saved == null || !saved.optBoolean("fight_keys_v15", false)) {
            // Correct just these three actions once, including installed layouts.
            // Geometry, D-pad and every other binding retain their saved values.
            keyIdx[SOCO] = DEFAULT_KEY[SOCO];
            keyIdx[CHUTE] = DEFAULT_KEY[CHUTE];
            keyIdx[AGARRAR] = DEFAULT_KEY[AGARRAR];
            // Persist after the first layout, when the default D-pad anchor
            // has its actual screen coordinates too.
            mappingMigrationPending = true;
        }
        if (saved == null || !saved.optBoolean("run_special_v16", false)) {
            keyIdx[CORRER] = DEFAULT_KEY[CORRER];
            keyIdx[ESPECIAL] = DEFAULT_KEY[ESPECIAL];
            mappingMigrationPending = true;
        }
        // This is now the only in-game editor shortcut, so it must remain
        // reachable even if an older layout hid its artwork.
        visible[CONFIG] = true;
    }

    public void setMenuOnly(boolean value) {
        if (menuOnly == value) return;
        releaseAll(); menuOnly = value; invalidate();
    }

    private Bitmap load(String name) {
        try {
            int id = getResources().getIdentifier(name, "drawable", getContext().getPackageName());
            if (id == 0) return null;
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inScaled = false;
            return BitmapFactory.decodeResource(getResources(), id, o);
        } catch (Throwable t) {
            return null;
        }
    }

    private static float clamp(float v, float lo, float hi) {
        return Float.isNaN(v) || Float.isInfinite(v) ? lo : Math.max(lo, Math.min(hi, v));
    }

    private float dp(float n) {
        return n * getResources().getDisplayMetrics().density;
    }

    private static boolean isStick(int i) { return i == STICK_L || i == STICK_R; }
    private static boolean isDpad(int i) { return i >= UP; }
    private static boolean isRect(int i) { return i >= UP; }

    /* ------------------------------------------------------------- geometry */

    private void place(int i, float ox, float oy, float ax, float ay, float bx, float by) {
        /* ax,ay: picture-space anchor of the group; bx,by: screen position of that anchor. */
        cx[i] = bx + (ox - ax) * k;
        cy[i] = by + (oy - ay) * k;
    }

    private void geometry() {
        if (w <= 0 || h <= 0) return;
        float s = userScale / .9f;
        k = h * .0006f * s;
        float margin = clamp(h * .035f, dp(8), dp(18));

        float rL = h * .125f * s;
        float rR = rL * .80f;
        cx[STICK_L] = margin + rL + dp(4);
        cy[STICK_L] = h - margin - rL;
        hw[STICK_L] = hh[STICK_L] = rL;
        cx[STICK_R] = w * .5f;
        cy[STICK_R] = h - margin - rR;
        hw[STICK_R] = hh[STICK_R] = rR;

        for (int i = SOCO; i <= CORRER; i++) {
            place(i, ART[i][0], ART[i][1], 1661f, 913f, w - margin, h - margin);
        }
        for (int i = PAUSA; i <= CONFIG; i++) {
            place(i, ART[i][0], ART[i][1], 836f, 7f, w * .5f, margin);
        }
        for (int i = UP; i <= RIGHT; i++) {
            place(i, ART[i][0], ART[i][1], 209f, 699f, margin + rL, h - margin - rL);
        }
        for (int i = SOCO; i < N; i++) {
            float m = k * mult[i];
            if (isRect(i)) { hw[i] = ART[i][2] * m * .5f; hh[i] = ART[i][3] * m * .5f; }
            else { hw[i] = ART[i][2] * m * .5f; hh[i] = ART[i][2] * m * .5f; }
        }
        for (int i = 0; i < 2; i++) { hw[i] *= mult[i]; hh[i] *= mult[i]; }
        for (int i = 0; i < N; i++) {
            if (hasPos[i]) {
                cx[i] = clamp(fx[i] * w, hw[i], Math.max(hw[i], w - hw[i]));
                cy[i] = clamp(fy[i] * h, hh[i], Math.max(hh[i], h - hh[i]));
            }
        }
        if (!dpad.positioned) {
            dpad.x = (margin + rL) / w;
            dpad.y = (h - margin - rL) / h;
        }
        dpad.layout(w, h, h * .075f * s);
        for (int i = UP; i <= RIGHT; i++) {
            int n = i - UP;
            cx[i] = dpad.cx[n]; cy[i] = dpad.cy[n];
            hw[i] = hh[i] = dpad.cell * .5f;
            visible[i] = dpad.visible; mult[i] = dpad.scale;
            hasPos[i] = false;
        }
    }

    @Override
    protected void onSizeChanged(int width, int height, int oldw, int oldh) {
        w = width;
        h = height;
        geometry();
        if (mappingMigrationPending && width > 0 && height > 0) save(null);
    }

    /* -------------------------------------------------------------- drawing */

    private void buildStickShaders(int i, float r) {
        if (Math.abs(shaderR[i] - r) < .5f && baseShader[i] != null) return;
        shaderR[i] = r;
        baseShader[i] = new RadialGradient(0, 0, r,
                new int[]{Color.rgb(44, 38, 30), Color.rgb(20, 18, 16), Color.rgb(8, 8, 10)},
                new float[]{0f, .72f, 1f}, Shader.TileMode.CLAMP);
        float kr = r * .42f;
        knobShader[i] = new RadialGradient(-kr * .28f, -kr * .34f, kr * 1.35f,
                new int[]{Color.rgb(250, 226, 160), Color.rgb(196, 144, 52), Color.rgb(64, 42, 14)},
                new float[]{0f, .50f, 1f}, Shader.TileMode.CLAMP);
    }

    private void drawStick(Canvas c, int i) {
        float r = hw[i];
        buildStickShaders(i, r);
        boolean on = pressed[i];
        float a = edit ? (visible[i] ? 1f : .28f) : (on ? 1f : spriteAlpha);
        int alpha = Math.round(255f * a);
        c.save();
        c.translate(cx[i], cy[i]);

        fillPaint.setShader(baseShader[i]);
        fillPaint.setAlpha(alpha);
        c.drawCircle(0, 0, r, fillPaint);
        fillPaint.setShader(null);

        strokePaint.setColor(Color.argb(alpha, 230, 174, 67));
        strokePaint.setStrokeWidth(Math.max(dp(2f), r * .05f));
        c.drawCircle(0, 0, r - strokePaint.getStrokeWidth() * .5f, strokePaint);
        strokePaint.setColor(Color.argb(Math.round(alpha * .35f), 230, 174, 67));
        strokePaint.setStrokeWidth(dp(1.2f));
        c.drawCircle(0, 0, r * .70f, strokePaint);

        fillPaint.setColor(Color.argb(Math.round(alpha * (on ? .95f : .75f)), 240, 196, 92));
        float cs = r * .09f;
        for (int q = 0; q < 4; q++) {
            c.save();
            c.rotate(q * 90f);
            chevron.reset();
            chevron.moveTo(0, -r * .84f - cs);
            chevron.lineTo(cs * 1.25f, -r * .84f + cs * .7f);
            chevron.lineTo(-cs * 1.25f, -r * .84f + cs * .7f);
            chevron.close();
            c.drawPath(chevron, fillPaint);
            c.restore();
        }

        float kr = r * .42f;
        float kx = knobX[i % 2], ky = knobY[i % 2];
        c.translate(kx, ky);
        fillPaint.setColor(Color.argb(Math.round(alpha * .55f), 0, 0, 0));
        c.drawCircle(kr * .10f, kr * .16f, kr * 1.04f, fillPaint);
        fillPaint.setShader(knobShader[i]);
        fillPaint.setAlpha(alpha);
        c.drawCircle(0, 0, kr, fillPaint);
        fillPaint.setShader(null);
        strokePaint.setColor(Color.argb(alpha, 54, 34, 8));
        strokePaint.setStrokeWidth(dp(1.6f));
        c.drawCircle(0, 0, kr, strokePaint);
        strokePaint.setColor(Color.argb(Math.round(alpha * .85f), 255, 232, 170));
        strokePaint.setStrokeWidth(dp(1.1f));
        c.drawCircle(0, 0, kr * .62f, strokePaint);
        c.restore();
    }

    private void drawSprite(Canvas c, int i) {
        Bitmap b = bmp[i];
        float m = k * mult[i] * (pressed[i] && !edit ? .93f : 1f);
        float a = edit ? (visible[i] ? 1f : .28f) : (pressed[i] ? 1f : spriteAlpha);
        if (b == null) {
            fillPaint.setColor(Color.argb(Math.round(255 * a), 30, 26, 20));
            c.drawCircle(cx[i], cy[i], hw[i], fillPaint);
            return;
        }
        float dw = ART[i][4] * m, dh = ART[i][5] * m;
        if (isDpad(i)) {
            dw = 2f * hw[i] * ART[i][4] / ART[i][2];
            dh = 2f * hh[i] * ART[i][5] / ART[i][3];
        }
        dst.set(cx[i] - dw * .5f, cy[i] - dh * .5f, cx[i] + dw * .5f, cy[i] + dh * .5f);
        spritePaint.setAlpha(Math.round(255f * a));
        c.drawBitmap(b, null, dst, spritePaint);
    }

    @Override
    protected void onDraw(Canvas c) {
        super.onDraw(c);
        geometry();
        for (int i = 0; i < N; i++) {
            if ((!visible[i] || (menuOnly && i != CONFIG)) && !edit) continue;
            if (isStick(i)) drawStick(c, i);
            else drawSprite(c, i);
        }
        if (edit && selected >= 0) {
            int i = selected;
            if (isDpad(i)) {
                dst.set(dpad.left - dp(5), dpad.top - dp(5), dpad.right + dp(5), dpad.bottom + dp(5));
                c.drawRoundRect(dst, dp(10), dp(10), selPaint);
            } else if (isStick(i) || (!isRect(i))) {
                c.drawCircle(cx[i], cy[i], hw[i] + dp(5), selPaint);
            } else {
                dst.set(cx[i] - hw[i] - dp(5), cy[i] - hh[i] - dp(5), cx[i] + hw[i] + dp(5), cy[i] + hh[i] + dp(5));
                c.drawRoundRect(dst, dp(10), dp(10), selPaint);
            }
        }
    }

    /* --------------------------------------------------------------- editor */

    public void setOnSelectionChanged(Runnable r) { onSelect = r; }
    public static String[] controlNames() { return java.util.Arrays.copyOf(NAMES, UP + 1); }
    public void selectControl(int index) {
        if (index < 0 || index >= N) return;
        selected = isDpad(index) ? UP : index;
        if (onSelect != null) onSelect.run();
        invalidate();
    }

    public String selectedInfo() {
        if (selected < 0) return null;
        if (isDpad(selected)) return "D-PAD • mover e redimensionar junto" + (dpad.visible ? "" : " (oculto)");
        StringBuilder sb = new StringBuilder(NAMES[selected]);
        if (keyIdx[selected] >= 0 && !isStick(selected) && !isDpad(selected))
            sb.append(" → ").append(KEY_LABEL[keyIdx[selected]]);
        sb.append(visible[selected] ? "" : "  (oculto)");
        return sb.toString();
    }

    public void scaleSelected(float factor) {
        if (selected < 0) return;
        if (isDpad(selected)) dpad.scale = clamp(dpad.scale * factor, .5f, 2.2f);
        else mult[selected] = clamp(mult[selected] * factor, .5f, 2.2f);
        geometry();
        invalidate();
    }

    public void cycleKey() {
        if (selected < 0 || isStick(selected) || isDpad(selected)) return;
        keyIdx[selected] = (keyIdx[selected] + 1) % KEY_CODE.length;
        invalidate();
        if (onSelect != null) onSelect.run();
    }

    public void toggleVisible() {
        if (selected < 0) return;
        if (selected == CONFIG) return; // keep the only in-game editor shortcut reachable
        if (isDpad(selected)) dpad.visible = !dpad.visible;
        else visible[selected] = !visible[selected];
        geometry();
        invalidate();
        if (onSelect != null) onSelect.run();
    }

    public void resetAll() {
        dpad.positioned = false; dpad.scale = 1; dpad.visible = true;
        for (int i = 0; i < N; i++) {
            hasPos[i] = false;
            mult[i] = 1f;
            visible[i] = !isStick(i);
            keyIdx[i] = DEFAULT_KEY[i];
        }
        selected = -1;
        geometry();
        invalidate();
        if (onSelect != null) onSelect.run();
    }

    private AtomicFile layoutFile() {
        return new AtomicFile(new File(getContext().getFilesDir(), "controls-v7.json"));
    }

    private JSONObject readLayout() {
        try {
            return new JSONObject(new String(layoutFile().readFully(), StandardCharsets.UTF_8));
        } catch (java.io.FileNotFoundException missing) { return null; }
        catch (Exception error) {
            android.util.Log.w("DefJamControls", "Cannot read layout; using legacy defaults", error);
            return null;
        }
    }

    public boolean save(SharedPreferences unused) {
        geometry();
        AtomicFile file = layoutFile();
        FileOutputStream output = null;
        try {
            JSONObject data = new JSONObject();
            data.put("controls_v7_arrows", true);
            data.put("fight_keys_v15", true);
            data.put("run_special_v16", true);
            data.put("dpad_v8", true);
            data.put("dpad_x", dpad.x); data.put("dpad_y", dpad.y);
            data.put("dpad_scale", dpad.scale); data.put("dpad_visible", dpad.visible);
            for (int i = 0; i < N; i++) {
                if (hasPos[i]) {
                    data.put("c6_" + i + "_x", fx[i]);
                    data.put("c6_" + i + "_y", fy[i]);
                }
                data.put("c6_" + i + "_s", Math.round(mult[i] * 100f));
                data.put("c6_" + i + "_v", visible[i]);
                data.put("c6_" + i + "_k", keyIdx[i]);
            }
            output = file.startWrite();
            output.write(data.toString().getBytes(StandardCharsets.UTF_8));
            file.finishWrite(output);
            mappingMigrationPending = false;
            return true;
        } catch (Exception error) {
            if (output != null) file.failWrite(output);
            android.util.Log.e("DefJamControls", "Layout save failed", error);
            return false;
        }
    }

    private int pick(float x, float y) {
        if (dpad.contains(x, y)) return UP;
        int best = -1;
        float bestD = Float.MAX_VALUE;
        for (int i = 0; i < N; i++) {
            float dx = Math.abs(x - cx[i]);
            float dy = Math.abs(y - cy[i]);
            if (dx > hw[i] + dp(8) || dy > hh[i] + dp(8)) continue;
            float d = (dx * dx) / (hw[i] * hw[i] + 1f) + (dy * dy) / (hh[i] * hh[i] + 1f);
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }

    private boolean editTouch(MotionEvent e) {
        int action = e.getActionMasked();
        float x = e.getX(), y = e.getY();
        geometry();
        if (action == MotionEvent.ACTION_DOWN) {
            int hit = pick(x, y);
            if (hit >= 0) {
                selected = isDpad(hit) ? UP : hit;
                grabDx = (isDpad(hit) ? dpad.x * w : cx[hit]) - x;
                grabDy = (isDpad(hit) ? dpad.y * h : cy[hit]) - y;
                dragging = true;
                if (onSelect != null) onSelect.run();
                invalidate();
            } else {
                dragging = false;
            }
            return true;
        }
        if (action == MotionEvent.ACTION_MOVE && dragging && selected >= 0 && w > 0 && h > 0) {
            int i = selected;
            if (isDpad(i)) {
                dpad.x = (x + grabDx) / w; dpad.y = (y + grabDy) / h;
                dpad.positioned = true; geometry(); invalidate(); return true;
            }
            float nx = clamp(x + grabDx, hw[i], Math.max(hw[i], w - hw[i]));
            float ny = clamp(y + grabDy, hh[i], Math.max(hh[i], h - hh[i]));
            fx[i] = nx / w;
            fy[i] = ny / h;
            hasPos[i] = true;
            invalidate();
            return true;
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) dragging = false;
        return true;
    }

    /* ----------------------------------------------------------------- play */

    private void stickKeys(Set<Integer> keys, int i, float x, float y) {
        float r = hw[i];
        float dx = x - cx[i], dy = y - cy[i];
        float len = (float)Math.sqrt(dx * dx + dy * dy);
        float lim = r * .6f;
        float sx = dx, sy = dy;
        if (len > lim && len > 0f) { sx = dx / len * lim; sy = dy / len * lim; }
        knobX[i] = sx;
        knobY[i] = sy;
        float nx = dx / r, ny = dy / r;
        float ax = Math.abs(nx), ay = Math.abs(ny);
        boolean hor = ax > deadzone, ver = ay > deadzone;
        if (hor && ver) {
            if (ay < ax * .45f) ver = false;
            else if (ax < ay * .45f) hor = false;
        }
        int left = i == STICK_L ? KeyEvent.KEYCODE_A : KeyEvent.KEYCODE_DPAD_LEFT;
        int right = i == STICK_L ? KeyEvent.KEYCODE_D : KeyEvent.KEYCODE_DPAD_RIGHT;
        int up = i == STICK_L ? KeyEvent.KEYCODE_W : KeyEvent.KEYCODE_DPAD_UP;
        int down = i == STICK_L ? KeyEvent.KEYCODE_S : KeyEvent.KEYCODE_DPAD_DOWN;
        if (hor) keys.add(nx < 0 ? left : right);
        if (ver) keys.add(ny < 0 ? up : down);
    }

    private boolean inside(int i, float x, float y) {
        float dx = x - cx[i], dy = y - cy[i];
        if (isRect(i)) return Math.abs(dx) <= hw[i] && Math.abs(dy) <= hh[i];
        float r = hw[i] * (i == ESPECIAL ? .92f : 1f);
        return dx * dx + dy * dy <= r * r;
    }

    private void evaluate(int pid, float x, float y, Set<Integer> keys, Set<Integer> ctls) {
        Integer owned = stickOwner.get(pid);
        if (owned == null && dpad.visible && dpad.contains(x, y)) {
            int directions = dpad.directionsAt(x, y);
            for (int i = UP; i <= RIGHT; i++) {
                if ((directions & (1 << (i - UP))) != 0) { keys.add(DPAD_KEY[i]); ctls.add(i); }
            }
            return;
        }
        if (owned == null) {
            for (int i = 0; i < 2; i++) {
                if (!visible[i]) continue;
                float dx = x - cx[i], dy = y - cy[i];
                float r = hw[i] * 1.15f;
                if (dx * dx + dy * dy <= r * r) { owned = i; stickOwner.put(pid, i); break; }
            }
        }
        if (owned != null) {
            stickKeys(keys, owned, x, y);
            ctls.add(owned);
            return;
        }
        int best = -1;
        float bestD = Float.MAX_VALUE;
        for (int i = SOCO; i < N; i++) {
            if (!visible[i] || !inside(i, x, y)) continue;
            float dx = (x - cx[i]) / (hw[i] + 1f), dy = (y - cy[i]) / (hh[i] + 1f);
            float d = dx * dx + dy * dy;
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best >= 0) {
            ctls.add(best);
            if (isDpad(best)) keys.add(DPAD_KEY[best]);
            else if (best != CONFIG && keyIdx[best] >= 0) keys.add(KEY_CODE[keyIdx[best]]);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (edit) return editTouch(e);
        geometry();
        int action = e.getActionMasked();
        int changed = e.getActionIndex();
        if ((action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN)
                && activity != null && visible[CONFIG]
                && inside(CONFIG, e.getX(changed), e.getY(changed))) {
            releaseAll();
            activity.openControlsEditor();
            return true;
        }
        // A transparent full-screen view must not eat A/B, the D-pad or
        // port touch when it is drawing only the editor wrench.
        if (menuOnly) return false;
        if (action == MotionEvent.ACTION_CANCEL) {
            releaseAll();
            return true;
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            int pid = e.getPointerId(changed);
            Integer owned = stickOwner.remove(pid);
            if (owned != null) { knobX[owned] = 0; knobY[owned] = 0; }
            pointerKeys.remove(pid);
            pointerCtls.remove(pid);
            syncKeys();
            if (action == MotionEvent.ACTION_UP) releaseAll();
            return true;
        }
        for (int i = 0; i < e.getPointerCount(); i++) {
            int pid = e.getPointerId(i);
            Set<Integer> keys = new HashSet<>();
            Set<Integer> ctls = new HashSet<>();
            evaluate(pid, e.getX(i), e.getY(i), keys, ctls);
            pointerKeys.put(pid, keys);
            pointerCtls.put(pid, ctls);
        }
        syncKeys();
        return true;
    }

    private void syncKeys() {
        Set<Integer> wanted = new HashSet<>();
        for (Set<Integer> keys : pointerKeys.values()) wanted.addAll(keys);
        for (int key : new HashSet<>(held)) if (!wanted.contains(key)) send(key, false);
        for (int key : wanted) if (!held.contains(key)) send(key, true);
        held.clear();
        held.addAll(wanted);
        for (int i = 0; i < N; i++) pressed[i] = false;
        for (Set<Integer> ctls : pointerCtls.values()) for (int i : ctls) pressed[i] = true;
        invalidate();
    }

    private void send(int key, boolean down) {
        if (activity == null) return;
        long now = android.os.SystemClock.uptimeMillis();
        activity.dispatchKeyEvent(new KeyEvent(now, now,
                down ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP, key, 0));
        if (down && hapticAmplitude > 0 && vibrator != null && now - lastHaptic >= 35) {
            lastHaptic = now;
            if (Build.VERSION.SDK_INT >= 26)
                vibrator.vibrate(VibrationEffect.createOneShot(9, hapticAmplitude));
            else
                vibrator.vibrate(9);
        }
    }

    public void releaseAll() {
        for (int key : new HashSet<>(held)) send(key, false);
        held.clear();
        pointerKeys.clear();
        pointerCtls.clear();
        stickOwner.clear();
        for (int i = 0; i < N; i++) pressed[i] = false;
        knobX[0] = knobY[0] = knobX[1] = knobY[1] = 0f;
        postInvalidate();
    }
}
