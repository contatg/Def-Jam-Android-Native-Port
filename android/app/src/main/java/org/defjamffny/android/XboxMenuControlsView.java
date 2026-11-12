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
    private final TouchDpadLayout dpad=new TouchDpadLayout();
    private final String[] names={"A","B","X","Y","LT","RT","LB","RB"};
    private final int[] bindings={1,3,0,2,6,7,4,5};
    private final int[] keys={KeyEvent.KEYCODE_K,KeyEvent.KEYCODE_L,KeyEvent.KEYCODE_J,KeyEvent.KEYCODE_I,KeyEvent.KEYCODE_Q,KeyEvent.KEYCODE_E,KeyEvent.KEYCODE_U,KeyEvent.KEYCODE_O};
    private final int[] settingsIndex={MenuTouchSettings.A,MenuTouchSettings.B,MenuTouchSettings.X,MenuTouchSettings.Y,MenuTouchSettings.LT,MenuTouchSettings.RT,MenuTouchSettings.LB,MenuTouchSettings.RB};
    private boolean onlineMode;
    private final int[] directions={KeyEvent.KEYCODE_NUMPAD_8,KeyEvent.KEYCODE_NUMPAD_2,
            KeyEvent.KEYCODE_NUMPAD_4,KeyEvent.KEYCODE_NUMPAD_6};
    private final RectF[] boxes=new RectF[names.length];
    private final Map<Integer,Set<Integer>> pointers=new HashMap<>();
    private final Set<Integer> held=new HashSet<>();
    private static final String[] ARROWS={"▲","▼","◀","▶"};
    private final RectF arrow=new RectF();
    private float radius;
    private final MenuTouchSettings settings;
    public XboxMenuControlsView(DefJamActivity context){
        super(context);activity=context;setFocusable(false);settings=MenuTouchSettings.read(context);
        setContentDescription(UiText.t("Use A, B and the D-pad to navigate menus"));
        for(int i=0;i<boxes.length;i++)boxes[i]=new RectF();
    }
    private float dp(float v){return v*getResources().getDisplayMetrics().density;}
    public void setOnlineMode(boolean value){
        if(onlineMode==value)return;
        releaseAll();onlineMode=value;onSizeChanged(getWidth(),getHeight(),getWidth(),getHeight());invalidate();
    }
    private int count(){return names.length;}
    @Override protected void onSizeChanged(int w,int h,int oldw,int oldh){
        releaseAll();radius=Math.min(h*.068f,w*.04f);
        for(int i=0;i<boxes.length;i++){
            int index=settingsIndex[i];
            float hw=radius*settings.size[index],hh=hw;
            if(i>=4){hw*=1.1f;hh*=.65f;}
            float x=MenuTouchSettings.center(settings.x[index],w,hw);
            float y=MenuTouchSettings.center(settings.y[index],h,hh);
            boxes[i].set(x-hw,y-hh,x+hw,y+hh);
        }
        dpad.x=settings.x[2];dpad.y=settings.y[2];dpad.scale=settings.size[2];dpad.layout(w,h,h*.075f);
    }
    private void label(Canvas c,String text,float x,float y,float size,int color){
        paint.setStyle(Paint.Style.FILL);paint.setColor(color);paint.setTextSize(size);
        paint.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);paint.setTextAlign(Paint.Align.CENTER);
        c.drawText(text,x,y-(paint.ascent()+paint.descent())*.5f,paint);
    }
    private int faded(int color,float alpha) {
        return (color & 0x00ffffff) | (Math.round(Color.alpha(color)*alpha) << 24);
    }
    @Override protected void onDraw(Canvas c){
        for(int i=0;i<count();i++){
            int index=settingsIndex[i];
            RectF r=boxes[i];paint.setStyle(Paint.Style.FILL);paint.setColor(faded(Color.argb(180,16,18,22),settings.alpha[index]));
            if(i<4)c.drawOval(r,paint);else c.drawRoundRect(r,dp(10),dp(10),paint);
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(dp(2));paint.setColor(faded(ControllerGlyphs127.color(bindings[i],settings.playStation),settings.alpha[index]));
            if(i<4)c.drawOval(r,paint);else c.drawRoundRect(r,dp(10),dp(10),paint);
            label(c,ControllerGlyphs127.label(bindings[i],settings.playStation),r.centerX(),r.centerY(),Math.min(dp(22)*settings.size[index],r.width()*.35f),faded(ControllerGlyphs127.color(bindings[i],settings.playStation),settings.alpha[index]));
        }
        for(int i=0;i<4;i++){
            float half=dpad.cell*.48f;arrow.set(dpad.cx[i]-half,dpad.cy[i]-half,dpad.cx[i]+half,dpad.cy[i]+half);RectF r=arrow;
            paint.setColor(faded(Color.argb(190,22,23,27),settings.alpha[2]));paint.setStyle(Paint.Style.FILL);c.drawRoundRect(r,dp(9),dp(9),paint);
            label(c,ARROWS[i],r.centerX(),r.centerY(),dpad.cell*.5f,faded(Color.rgb(230,174,67),settings.alpha[2]));
        }
    }
    private Set<Integer> keysAt(float x,float y){
        Set<Integer> set=new HashSet<>();
        int flags=dpad.directionsAt(x,y);
        for(int i=0;i<4;i++)if((flags&(1<<i))!=0)set.add(directions[i]);
        for(int i=0;i<count();i++)if(boxes[i].contains(x,y)){set.add(keys[i]);break;}
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
    private void send(int key,boolean down){activity.sendMenuKey(key,down);}
    public void releaseAll(){for(int key:new HashSet<>(held))send(key,false);held.clear();pointers.clear();}
    @Override protected void onDetachedFromWindow(){releaseAll();super.onDetachedFromWindow();}
}
