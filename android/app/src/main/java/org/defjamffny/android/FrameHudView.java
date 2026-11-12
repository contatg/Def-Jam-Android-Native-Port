package org.defjamffny.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.View;
import java.util.Locale;

/** Native accepted-present measurements. Never estimates FPS from display Hz. */
final class FrameHudView extends View {
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private String rate="-- FPS",detail="-- ms  |  p99 --";
    FrameHudView(Context context){super(context);setFocusable(false);setClickable(false);}
    private float dp(float n){return n*getResources().getDisplayMetrics().density;}
    void update(float[] values){
        if(values==null||values.length<6)return;
        float fps=values[0],mean=values[1],p99=values[3];
        rate=fps>.1f?String.format(Locale.US,"%.1f FPS",fps):"-- FPS";
        detail=mean<=0?"-- ms  |  p99 --":String.format(Locale.US,"%.1f ms  |  p99 %s",mean,Float.isInfinite(p99)?">800":String.format(Locale.US,"%.1f",p99));
        if(values.length>=10&&values[6]>.5f){
            rate=String.format(Locale.US,"Real %.0f | Saída %.0f",values[0],values[8]);
            detail=String.format(Locale.US,"FG únicos %.0f/s · envios",values[9]);
        }
        setContentDescription(rate+", "+detail);invalidate();
    }
    @Override protected void onDraw(Canvas c){
        super.onDraw(c);float w=getWidth(),h=getHeight();
        paint.setStyle(Paint.Style.FILL);paint.setColor(0xe8101318);c.drawRoundRect(0,0,w,h,dp(12),dp(12),paint);
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(dp(1));paint.setColor(0xff8a713c);c.drawRoundRect(dp(.5f),dp(.5f),w-dp(.5f),h-dp(.5f),dp(12),dp(12),paint);
        paint.setStyle(Paint.Style.FILL);paint.setTypeface(android.graphics.Typeface.create("sans-serif-medium",0));paint.setTextSize(dp(19));
        paint.setColor(0xffe5e9ed);if(paint.measureText(rate)>w-dp(24))paint.setTextSize(Math.max(dp(10),paint.getTextSize()*(w-dp(24))/paint.measureText(rate)));c.drawText(rate,dp(12),dp(25),paint);
        paint.setColor(0xffb8bec7);paint.setTextSize(dp(10));c.drawText(detail,dp(12),dp(42),paint);
    }
}
