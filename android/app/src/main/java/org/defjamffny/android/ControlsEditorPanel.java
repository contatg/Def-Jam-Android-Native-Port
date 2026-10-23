package org.defjamffny.android;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Same-activity editor: never replaces or destroys the SDL surface. */
public final class ControlsEditorPanel extends FrameLayout {
    private VirtualControlsView view;
    private TextView hint;
    private int dp(float n) {
        return Math.round(n * getResources().getDisplayMetrics().density);
    }

    private Button button(String label, boolean primary) {
        Button b = new Button(getContext());
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(12);
        b.setTextColor(primary ? Color.rgb(16, 18, 22) : Color.WHITE);
        b.setMinWidth(0);
        b.setMinimumWidth(0);
        b.setPadding(dp(12), 0, dp(12), 0);
        GradientDrawable d = new GradientDrawable();
        d.setColor(primary ? Color.rgb(230, 174, 67) : Color.argb(235, 40, 44, 52));
        d.setCornerRadius(dp(10));
        d.setStroke(dp(1), Color.argb(200, 230, 174, 67));
        b.setBackground(d);
        return b;
    }

    private LinearLayout row(View... items) {
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER);
        for (int i = 0; i < items.length; i++) {
            LinearLayout.LayoutParams q = new LinearLayout.LayoutParams(-2, dp(36));
            if (i > 0) q.leftMargin = dp(6);
            row.addView(items[i], q);
        }
        return row;
    }

    public ControlsEditorPanel(Context context, Runnable onSaved, Runnable onCanceled) {
        super(context);
        FrameLayout root = this;
        view = new VirtualControlsView(context, null, true);
        root.addView(view, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout bar = new LinearLayout(getContext());
        bar.setOrientation(LinearLayout.VERTICAL);
        bar.setGravity(Gravity.CENTER_HORIZONTAL);
        bar.setPadding(dp(10), dp(8), dp(10), dp(8));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.argb(225, 10, 11, 14));
        bg.setCornerRadius(dp(14));
        bg.setStroke(dp(1), Color.argb(205, 230, 174, 67));
        bar.setBackground(bg);

        hint = new TextView(getContext());
        hint.setTextColor(Color.WHITE);
        hint.setTextSize(12);
        hint.setGravity(Gravity.CENTER);
        bar.addView(hint, new LinearLayout.LayoutParams(-2, -2));

        Button minus = button("  −  ", false);
        Button plus = button("  +  ", false);
        Button map = button("Trocar botão", false);
        Button vis = button("Mostrar/ocultar", false);
        Button choose = button("Escolher", false);
        Button reset = button("Padrão", false);
        Button cancel = button("Cancelar", false);
        Button save = button("Salvar", true);

        LinearLayout.LayoutParams r1 = new LinearLayout.LayoutParams(-2, -2);
        r1.topMargin = dp(6);
        bar.addView(row(minus, plus, map, vis), r1);
        LinearLayout.LayoutParams r2 = new LinearLayout.LayoutParams(-2, -2);
        r2.topMargin = dp(6);
        bar.addView(row(choose, reset, cancel, save), r2);

        FrameLayout.LayoutParams bp = new FrameLayout.LayoutParams(-2, -2,
                Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        bp.topMargin = dp(10);
        root.addView(bar, bp);


        minus.setOnClickListener(v -> view.scaleSelected(1f / 1.1f));
        plus.setOnClickListener(v -> view.scaleSelected(1.1f));
        map.setOnClickListener(v -> view.cycleKey());
        vis.setOnClickListener(v -> view.toggleVisible());
        choose.setOnClickListener(v -> new AlertDialog.Builder(context).setTitle("Selecionar controle")
                .setItems(VirtualControlsView.controlNames(), (d, which) -> view.selectControl(which)).show());
        reset.setOnClickListener(v -> view.resetAll());
        cancel.setOnClickListener(v -> onCanceled.run());
        save.setOnClickListener(v -> {
            SharedPreferences p = context.getSharedPreferences("launcher", Context.MODE_PRIVATE);
            if (view.save(p)) onSaved.run();
            else android.widget.Toast.makeText(context, "Não foi possível salvar. Tente novamente.", android.widget.Toast.LENGTH_LONG).show();
        });
        view.setOnSelectionChanged(this::updateHint);
        updateHint();
    }

    private void updateHint() {
        String n = view.selectedInfo();
        hint.setText(n == null
                ? "Toque num controle e arraste. − e + mudam o tamanho; \"Trocar botão\" muda qual botão do jogo ele aperta."
                : n + "  •  arraste para mover");
    }

}
