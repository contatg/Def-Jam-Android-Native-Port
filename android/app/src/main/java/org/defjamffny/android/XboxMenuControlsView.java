package org.defjamffny.android;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/** Menu-only Xbox bindings. Never reads or changes the custom fight layout. */
public final class XboxMenuControlsView extends View {
    private final DefJamActivity activity;
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final DpadLayout dpad=new DpadLayout();
    private final String[] names={"A","B"};
    private final int[] keys={KeyEvent.KEYCODE_K,KeyEvent.KEYCODE_L};
    private final int[] directions={KeyEvent.KEYCODE_NUMPAD_8,KeyEvent.KEYCODE_NUMPAD_2,
            KeyEvent.KEYCODE_NUMPAD_4,KeyEvent.KEYCODE_NUMPAD_6};
    private final RectF[] boxes=new RectF[names.length];
    private final Map<Integer,Set<Integer>> pointers=new HashMap<>();
    private final Set<Integer> held=new HashSet<>();
    private float radius;
    public XboxMenuControlsView(DefJamActivity context){
        super(context);activity=context;setFocusable(false);
        setContentDescription("Use A, B and the D-pad to navigate menus");
        for(int i=0;i<boxes.length;i++)boxes[i]=new RectF();
    }
    private float dp(float v){return v*getResources().getDisplayMetrics().density;}
    @Override protected void onSizeChanged(int w,int h,int oldw,int oldh){
        releaseAll();radius=Math.min(h*.068f,w*.04f);
        float[] xs={.86f,.95f};
        float[] ys={.83f,.83f};
        for(int i=0;i<boxes.length;i++){
            float hw=i<4?radius:radius*1.15f,hh=i<4?radius:radius*.55f;
            boxes[i].set(xs[i]*w-hw,ys[i]*h-hh,xs[i]*w+hw,ys[i]*h+hh);
        }
        dpad.x=.14f;dpad.y=.76f;dpad.layout(w,h,h*.075f);
    }
    private void label(Canvas c,String text,float x,float y,float size,int color){
        paint.setStyle(Paint.Style.FILL);paint.setColor(color);paint.setTextSize(size);
        paint.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);paint.setTextAlign(Paint.Align.CENTER);
        c.drawText(text,x,y-(paint.ascent()+paint.descent())*.5f,paint);
    }
    @Override protected void onDraw(Canvas c){
        int[] colors={Color.rgb(88,214,123),Color.rgb(242,91,91),Color.rgb(77,166,255),Color.rgb(246,196,68)};
        for(int i=0;i<boxes.length;i++){
            RectF r=boxes[i];paint.setStyle(Paint.Style.FILL);paint.setColor(Color.argb(180,16,18,22));
            if(i<4)c.drawOval(r,paint);else c.drawRoundRect(r,dp(10),dp(10),paint);
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(dp(2));paint.setColor(i<4?colors[i]:Color.rgb(230,174,67));
            if(i<4)c.drawOval(r,paint);else c.drawRoundRect(r,dp(10),dp(10),paint);
            label(c,names[i],r.centerX(),r.centerY(),Math.min(dp(i<4?22:12),radius*.7f),i<4?colors[i]:Color.WHITE);
        }
        String[] arrows={"▲","▼","◀","▶"};
        for(int i=0;i<4;i++){
            float half=dpad.cell*.48f;RectF r=new RectF(dpad.cx[i]-half,dpad.cy[i]-half,dpad.cx[i]+half,dpad.cy[i]+half);
            paint.setColor(Color.argb(190,22,23,27));paint.setStyle(Paint.Style.FILL);c.drawRoundRect(r,dp(9),dp(9),paint);
            label(c,arrows[i],r.centerX(),r.centerY(),dpad.cell*.5f,Color.rgb(230,174,67));
        }
    }
    private Set<Integer> keysAt(float x,float y){
        Set<Integer> set=new HashSet<>();
        int flags=dpad.directionsAt(x,y);
        for(int i=0;i<4;i++)if((flags&(1<<i))!=0)set.add(directions[i]);
        for(int i=0;i<boxes.length;i++)if(boxes[i].contains(x,y)){set.add(keys[i]);break;}
        return set;
    }
    @Override public boolean onTouchEvent(MotionEvent e){
        int action=e.getActionMasked(),changed=e.getActionIndex();
        if(action==MotionEvent.ACTION_CANCEL){releaseAll();return true;}
        if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_POINTER_UP)pointers.remove(e.getPointerId(changed));
        for(int i=0;i<e.getPointerCount();i++){
            if((action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_POINTER_UP)&&i==changed)continue;
            pointers.put(e.getPointerId(i),keysAt(e.getX(i),e.getY(i)));
        }
        Set<Integer> wanted=new HashSet<>();for(Set<Integer> set:pointers.values())wanted.addAll(set);
        for(int key:new HashSet<>(held))if(!wanted.contains(key))send(key,false);
        for(int key:wanted)if(!held.contains(key))send(key,true);
        held.clear();held.addAll(wanted);if(action==MotionEvent.ACTION_UP)releaseAll();return true;
    }
    private void send(int key,boolean down){long now=android.os.SystemClock.uptimeMillis();activity.dispatchKeyEvent(new KeyEvent(now,now,down?KeyEvent.ACTION_DOWN:KeyEvent.ACTION_UP,key,0));}
    public void releaseAll(){for(int key:new HashSet<>(held))send(key,false);held.clear();pointers.clear();}
    @Override protected void onDetachedFromWindow(){releaseAll();super.onDetachedFromWindow();}
}
