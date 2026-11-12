package org.defjamffny.android;

import android.app.Dialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.text.InputFilter;
import android.text.InputType;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** Name/keyboard entry only. Rooms and matches stay in the current native UI. */
final class OnlineNamePrompt129 {
    interface Entry { boolean open(String player); }
    private final DefJamActivity activity;
    private Dialog dialog;

    OnlineNamePrompt129(DefJamActivity activity) { this.activity = activity; }
    private int dp(float n) { return PortUi.dp(activity, n); }
    void show(Entry entry) {
        if (dialog != null || activity.isFinishing() || activity.isDestroyed()) return;
        SharedPreferences preferences = activity.getSharedPreferences("online", Context.MODE_PRIVATE);
        final Runnable[] confirm = new Runnable[1];
        Dialog current = new Dialog(activity, android.R.style.Theme_Material_Dialog_NoActionBar) {
            private int confirmKey = -1, cancelKey = -1;
            @Override public boolean dispatchKeyEvent(KeyEvent event) {
                int key = event.getKeyCode();
                if (key == KeyEvent.KEYCODE_BUTTON_A || key == KeyEvent.KEYCODE_BUTTON_START
                        || key == KeyEvent.KEYCODE_ENTER) {
                    // Ignore the release of the A/Enter that opened this form.
                    if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) confirmKey = key;
                    if (event.getAction() == KeyEvent.ACTION_UP && confirmKey == key) {
                        confirmKey = -1;
                        if (confirm[0] != null) confirm[0].run();
                    }
                    return true;
                }
                if (key == KeyEvent.KEYCODE_BUTTON_B || key == KeyEvent.KEYCODE_ESCAPE) {
                    if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) cancelKey = key;
                    if (event.getAction() == KeyEvent.ACTION_UP && cancelKey == key) { cancelKey = -1; dismiss(); }
                    return true;
                }
                return super.dispatchKeyEvent(event);
            }
        };
        dialog = current;
        current.setCanceledOnTouchOutside(false);
        LinearLayout body = new LinearLayout(activity);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(22), dp(18), dp(22), dp(18));
        body.setBackground(PortUi.shape(activity, PortUi.CARD, PortUi.LINE, 18));
        add(body, PortUi.text(activity, "Enter online", 24, PortUi.TEXT, true), 0);
        TextView detail = PortUi.text(activity, "Choose the name shown in rooms and matches.", 13, PortUi.MUTED, false);
        add(body, detail, 8);
        TextView label = PortUi.text(activity, "Player name", 12, PortUi.GOLD, true);
        add(body, label, 18);
        EditText name = new EditText(activity);
        name.setId(View.generateViewId());
        label.setLabelFor(name.getId());
        name.setContentDescription(UiText.t("Player name"));
        name.setSingleLine(true);
        name.setTextSize(18);
        name.setTextColor(PortUi.TEXT);
        name.setHintTextColor(PortUi.MUTED);
        name.setPadding(dp(14), dp(12), dp(14), dp(12));
        name.setMinimumHeight(dp(52));
        name.setBackground(PortUi.shape(activity, PortUi.BG, PortUi.GOLD, 10));
        name.setFilters(new InputFilter[]{new InputFilter.LengthFilter(24)});
        name.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        name.setImeOptions(EditorInfo.IME_ACTION_GO | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        String signedName133=ZeroTier148.name(activity);
        name.setText(signedName133.isEmpty()?OnlinePlayerName129.read(preferences):signedName133);
        name.setSelectAllOnFocus(true);
        add(body, name, 6);
        add(body, PortUi.text(activity, "Your name is saved for the next session.", 12, PortUi.MUTED, false), 8);
        LinearLayout actions = new LinearLayout(activity);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        actions.setGravity(Gravity.CENTER_VERTICAL);
        Button cancel = PortUi.button(activity, "Cancel", false);
        Button enter = PortUi.button(activity, "Enter online", true);
        LinearLayout.LayoutParams first = new LinearLayout.LayoutParams(0, dp(48), 1);
        LinearLayout.LayoutParams second = new LinearLayout.LayoutParams(0, dp(48), 1);
        second.leftMargin = dp(10);
        actions.addView(cancel, first);
        actions.addView(enter, second);
        add(body, actions, 18);
        Button account133=PortUi.button(activity,"ZeroTier / Online",false);
        add(body,account133,8);
        account133.setOnClickListener(v->OnlineAccount133.show(activity,()->{
            String signed=ZeroTier148.name(activity);if(!signed.isEmpty())name.setText(signed);
        }));
        confirm[0] = () -> {
            if (dialog != current || !current.isShowing() || activity.isFinishing() || activity.isDestroyed()) return;
            String player = OnlinePlayerName129.normalize(name.getText().toString());
            if (!OnlineMenuState.validName(player)) {
                name.setError(UiText.t("Use 1 to 24 letters (A-Z), numbers, spaces, _ or -."));
                name.requestFocus();
                return;
            }
            if (!OnlinePlayerName129.save(preferences, player)) {
                name.setError(UiText.t("Could not save. Please try again."));
                return;
            }
            // A failed/menu-invalid entry keeps the field open and the chosen name saved.
            if (entry.open(player)) current.dismiss();
        };
        enter.setOnClickListener(v -> confirm[0].run());
        cancel.setOnClickListener(v -> current.dismiss());
        name.setOnEditorActionListener((v, action, event) -> {
            if (action == EditorInfo.IME_ACTION_GO || action == EditorInfo.IME_ACTION_DONE
                    || (event != null && event.getKeyCode() == KeyEvent.KEYCODE_ENTER
                    && event.getAction() == KeyEvent.ACTION_UP)) {
                confirm[0].run();
                return true;
            }
            return false;
        });
        ScrollView scroll = new ScrollView(activity);
        scroll.addView(body);
        current.setContentView(scroll);
        current.setOnDismissListener(ignored -> {
            InputMethodManager keyboard = (InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE);
            if (keyboard != null) keyboard.hideSoftInputFromWindow(name.getWindowToken(), 0);
            boolean restore = dialog == current;
            if (restore) dialog = null;
            if (restore && !activity.isFinishing() && !activity.isDestroyed()) activity.restoreAfterOnline();
        });
        current.show();
        Window window = current.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            int width = Math.max(1, Math.min(dp(520), activity.getResources().getDisplayMetrics().widthPixels - dp(24)));
            window.setLayout(width, WindowManager.LayoutParams.WRAP_CONTENT);
            window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE
                    | WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        }
        name.requestFocus();
        name.post(() -> {
            if (dialog != current || !current.isShowing() || !name.hasFocus()) return;
            InputMethodManager keyboard = (InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE);
            if (keyboard != null) keyboard.showSoftInput(name, InputMethodManager.SHOW_IMPLICIT);
        });
    }
    private void add(LinearLayout parent, View child, int top) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.topMargin = dp(top);
        parent.addView(child, params);
    }
    void dismiss() {
        Dialog current = dialog;
        dialog = null; // lifecycle close must not refresh input on a paused/destroyed Activity
        if (current != null) current.dismiss();
    }
}
