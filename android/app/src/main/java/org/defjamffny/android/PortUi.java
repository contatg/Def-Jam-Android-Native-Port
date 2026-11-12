package org.defjamffny.android;

import android.app.AlertDialog;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.view.Gravity;
import android.widget.Button;
import android.widget.TextView;

/** Small shared native palette for the control editor and game importer. */
final class PortUi {
    static final int BG=0xff0b0e14, CARD=0xff151c26, LINE=0xff303a48,
            GOLD=0xffe9b44b, TEXT=0xfff2f5fa, MUTED=0xffa6b0bf, GREEN=0xff70d4a6, RED=0xfff08288;
    static int dp(Context c,float v) { return Math.round(v*c.getResources().getDisplayMetrics().density); }
    static GradientDrawable shape(Context c,int color,int border,int radius) {
        GradientDrawable d=new GradientDrawable();d.setColor(color);d.setCornerRadius(dp(c,radius));
        if(border!=0)d.setStroke(dp(c,1),border);return d;
    }
    static TextView text(Context c,String label,float size,int color,boolean bold) {
        TextView t=new TextView(c);t.setText(UiText.t(label));t.setTextColor(color);t.setTextSize(size);
        t.setIncludeFontPadding(false);t.setTypeface(Typeface.create("sans-serif",bold?Typeface.BOLD:Typeface.NORMAL));
        return t;
    }
    static Button button(Context c,String label,boolean primary) {
        Button b=new Button(c);b.setText(UiText.t(label));b.setAllCaps(false);b.setTextSize(12);
        b.setTypeface(Typeface.create("sans-serif-medium",Typeface.NORMAL));
        b.setMinWidth(0);b.setMinimumWidth(0);b.setMinHeight(dp(c,38));b.setMinimumHeight(dp(c,38));
        b.setPadding(dp(c,12),dp(c,9),dp(c,12),dp(c,9));b.setSingleLine(true);
        b.setEllipsize(android.text.TextUtils.TruncateAt.END);b.setGravity(Gravity.CENTER);
        int normal=primary?GOLD:0xff202a37;
        StateListDrawable bg=new StateListDrawable();
        bg.addState(new int[]{-android.R.attr.state_enabled},shape(c,0xff1a222d,LINE,10));
        bg.addState(new int[]{android.R.attr.state_pressed},shape(c,primary?0xffffcf75:0xff344355,GOLD,10));
        bg.addState(new int[]{android.R.attr.state_focused},shape(c,normal,GOLD,12));
        bg.addState(new int[]{},shape(c,normal,primary?0:LINE,12));b.setBackground(bg);
        b.setBackgroundTintList(null);b.setElevation(0);b.setStateListAnimator(null);
        b.setTextColor(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled},new int[]{}},
                new int[]{0xff758090,primary?BG:TEXT}));
        return b;
    }
    static AlertDialog.Builder dialog(Context c) {
        return new AlertDialog.Builder(c,android.R.style.Theme_Material_Dialog_Alert);
    }
    static void show(AlertDialog.Builder builder) {
        AlertDialog d=builder.create();
        d.setOnShowListener(x->{
            for(int which:new int[]{AlertDialog.BUTTON_POSITIVE,AlertDialog.BUTTON_NEGATIVE}) {
                Button b=d.getButton(which);if(b!=null)b.setTextColor(GOLD);
            }
            if(d.getWindow()!=null)d.getWindow().setBackgroundDrawable(shape(d.getContext(),CARD,LINE,16));
        });d.show();
    }
    private PortUi() { }
}
