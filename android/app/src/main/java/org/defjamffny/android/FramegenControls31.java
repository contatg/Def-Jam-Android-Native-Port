package org.defjamffny.android;

import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.graphics.drawable.ClipDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.view.Gravity;
import android.view.Window;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

/** Explicit dark controls, independent of the application's legacy Holo widgets. */
final class FramegenControls31 {
    static final int GOLD=0xffe3bb73, DARK=0xff292d37, BORDER=0xff675538;
    static int dp(Context c,float n){return Math.round(n*c.getResources().getDisplayMetrics().density);}
    static GradientDrawable shape(Context c,int color,int border){
        GradientDrawable d=new GradientDrawable();d.setColor(color);d.setCornerRadius(dp(c,12));d.setStroke(dp(c,1),border);return d;
    }
    static void button(Button b,boolean selected){
        b.setBackgroundTintList(null);b.setBackground(shape(b.getContext(),selected?GOLD:DARK,BORDER));
        b.setTextColor(selected?0xff17191f:0xfff4e8d3);b.setTextSize(16);b.setTypeface(null,Typeface.NORMAL);
        b.setAllCaps(false);b.setMinHeight(dp(b.getContext(),50));b.setPadding(dp(b.getContext(),16),dp(b.getContext(),10),dp(b.getContext(),16),dp(b.getContext(),10));
    }
    static final class Toggle extends Button {
        private boolean checked;private Runnable changed;
        Toggle(Context c){super(c);setOnClickListener(v->setChecked(!checked));refresh();}
        private String label(){boolean pt=UiText.isPortuguese(),es=UiText.isSpanish();return checked?(pt?"ATIVADO  ✓":es?"ACTIVADO  ✓":"ENABLED  ✓"):(pt?"DESLIGADO  ·  toque para ativar":es?"DESACTIVADO  ·  toca para activar":"OFF  ·  tap to enable");}
        private void refresh(){button(this,checked);setText(label());setContentDescription(label());setAlpha(isEnabled()?1f:.4f);}
        boolean isChecked(){return checked;}
        void setChecked(boolean on){boolean diff=checked!=on;checked=on;refresh();if(diff&&changed!=null)changed.run();}
        void onChanged(Runnable listener){changed=listener;}
        @Override public void setEnabled(boolean on){super.setEnabled(on);setAlpha(on?1f:.4f);}
        @Override public void onInitializeAccessibilityNodeInfo(android.view.accessibility.AccessibilityNodeInfo info){super.onInitializeAccessibilityNodeInfo(info);info.setCheckable(true);info.setChecked(checked);}
    }
    static final class Choice extends Button {
        private final Activity owner;private final String title;private final String[] values;private int position;private Runnable changed;
        Choice(Activity a,String title,String[] values){super(a);owner=a;this.title=title;this.values=values.clone();button(this,false);setGravity(Gravity.START|Gravity.CENTER_VERTICAL);setSelection(0);setOnClickListener(v->choose());}
        int getSelectedItemPosition(){return position;}
        void setSelection(int p){int next=Math.max(0,Math.min(values.length-1,p));boolean diff=next!=position;position=next;setText(values[position]+"     ▾");setContentDescription(title+": "+values[position]);if(diff&&changed!=null)changed.run();}
        void onChanged(Runnable listener){changed=listener;}
        @Override public void setEnabled(boolean on){super.setEnabled(on);setAlpha(on?1f:.4f);}
        private void choose(){
            if(owner.isFinishing()||owner.isDestroyed())return;
            Dialog dialog=new Dialog(owner);dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
            LinearLayout box=new LinearLayout(owner);box.setOrientation(LinearLayout.VERTICAL);box.setPadding(dp(owner,16),dp(owner,14),dp(owner,16),dp(owner,14));box.setBackground(shape(owner,0xff20232c,BORDER));
            TextView heading=new TextView(owner);heading.setText(title);heading.setTextColor(GOLD);heading.setTextSize(18);heading.setPadding(dp(owner,8),dp(owner,6),dp(owner,8),dp(owner,14));box.addView(heading);
            for(int i=0;i<values.length;i++){
                final int selected=i;Button item=new Button(owner);button(item,i==position);item.setText(values[i]+(i==position?"   ✓":""));
                LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,-2);lp.topMargin=dp(owner,6);box.addView(item,lp);
                item.setOnClickListener(v->{setSelection(selected);dialog.dismiss();});
            }
            ScrollView scroll=new ScrollView(owner);scroll.addView(box);dialog.setContentView(scroll);dialog.show();
            Window w=dialog.getWindow();if(w!=null){w.setBackgroundDrawableResource(android.R.color.transparent);int width=owner.getResources().getDisplayMetrics().widthPixels;w.setLayout(Math.min(dp(owner,520),width-dp(owner,40)),ViewGroup.LayoutParams.WRAP_CONTENT);}
        }
    }
    static void slider(SeekBar bar){
        Context c=bar.getContext();GradientDrawable background=shape(c,0xff414652,0xff414652),progress=shape(c,GOLD,GOLD);
        ClipDrawable fill=new ClipDrawable(progress,Gravity.LEFT,ClipDrawable.HORIZONTAL);
        LayerDrawable layers=new LayerDrawable(new android.graphics.drawable.Drawable[]{background,fill});
        layers.setId(0,android.R.id.background);layers.setId(1,android.R.id.progress);
        for(int i=0;i<2;i++){layers.setLayerHeight(i,dp(c,5));layers.setLayerGravity(i,Gravity.CENTER_VERTICAL);}
        bar.setProgressDrawable(layers);bar.setProgressTintList(ColorStateList.valueOf(GOLD));bar.setProgressBackgroundTintList(ColorStateList.valueOf(0xff414652));
        GradientDrawable thumb=new GradientDrawable();thumb.setShape(GradientDrawable.OVAL);thumb.setColor(GOLD);thumb.setStroke(dp(c,3),0xfff8e6c3);thumb.setSize(dp(c,22),dp(c,22));bar.setThumb(thumb);bar.setThumbTintList(null);bar.setSplitTrack(false);bar.setMinHeight(dp(c,48));bar.setPadding(dp(c,12),0,dp(c,12),0);
    }
}
