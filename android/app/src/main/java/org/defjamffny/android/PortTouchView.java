package org.defjamffny.android;

import android.view.MotionEvent;
import android.view.View;
import org.libsdl.app.SDLActivity;

/** Direct single-finger input for the native port settings overlay. */
public final class PortTouchView extends View {
    private int pointer = -1, device;
    private float x, y;
    public PortTouchView(DefJamActivity context) {
        super(context); setFocusable(false);
        setContentDescription("Port menu: tap items and drag sliders to adjust or scroll.");
    }
    private float normalized(float v, int extent) {
        return Math.max(0f, Math.min(1f, extent > 1 ? v / (extent - 1f) : .5f));
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        int action=event.getActionMasked(), changed=event.getActionIndex();
        if(action==MotionEvent.ACTION_CANCEL){releaseTouch();return true;}
        if(action==MotionEvent.ACTION_DOWN){
            releaseTouch();pointer=event.getPointerId(changed);device=event.getDeviceId();
            x=normalized(event.getX(changed),getWidth());y=normalized(event.getY(changed),getHeight());
            SDLActivity.onNativeTouch(device,pointer,MotionEvent.ACTION_DOWN,x,y,1f);return true;
        }
        if(pointer<0)return true;
        int index=event.findPointerIndex(pointer);
        if(index>=0){x=normalized(event.getX(index),getWidth());y=normalized(event.getY(index),getHeight());}
        if((action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_POINTER_UP)
                && event.getPointerId(changed)==pointer){releaseTouch();return true;}
        if(action==MotionEvent.ACTION_MOVE)
            SDLActivity.onNativeTouch(device,pointer,MotionEvent.ACTION_MOVE,x,y,1f);
        return true;
    }
    public void releaseTouch(){
        if(pointer>=0){SDLActivity.onNativeTouch(device,pointer,MotionEvent.ACTION_UP,x,y,0f);pointer=-1;}
    }
    @Override protected void onDetachedFromWindow(){releaseTouch();super.onDetachedFromWindow();}
}
