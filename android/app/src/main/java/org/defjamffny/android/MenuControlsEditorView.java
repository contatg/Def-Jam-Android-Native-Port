package org.defjamffny.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/** Edit-only preview; it never sends input to the running game. */
final class MenuControlsEditorView extends View {
    private final MenuTouchSettings settings;
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF[] boxes=new RectF[MenuTouchSettings.COUNT];
    private final Bitmap[] artwork = new Bitmap[MenuTouchSettings.COUNT];
    private final Bitmap[] arrows = new Bitmap[4];
    private final Paint artworkPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final TouchEditorGuide editorGuide;
    private final TouchDpadLayout pad=new TouchDpadLayout();
    private final RectF arrow=new RectF();
    private static final String[] ARROWS={"▲","▼","◀","▶"};
    private int selected=MenuTouchSettings.A,owner=-1;
    private float dx,dy;
    private Runnable changed;
    MenuControlsEditorView(Context c,MenuTouchSettings values) {
        super(c);settings=values;
        editorGuide=new TouchEditorGuide(getResources().getDisplayMetrics().density);
        for (int i = 0; i < boxes.length; i++) {
            boxes[i] = new RectF();
            artwork[i] = VirtualControlsView.loadArtwork(c, MenuTouchSettings.artworkControl(i));
        }
        for (int i = 0; i < arrows.length; i++)
            arrows[i] = VirtualControlsView.loadArtwork(c, VirtualControlsView.UP + i);
    }
    private static String label129(int index){
        switch(index){case 0:return "A";case 1:return "B";case 2:return UiText.t("D-PAD (grouped)");case 3:return "ENTER / START";case 5:return "X / □";case 6:return "Y / △";case 7:return "LT / L2";case 8:return "RT / R2";case 9:return "LB / L1";case 10:return "RB / R1";default:return "";}
    }
    static String[] names(){String[] labels=new String[MenuTouchSettings.editorCount129()];for(int i=0;i<labels.length;i++)labels[i]=label129(MenuTouchSettings.editorIndex129(i));return labels;}
    String selectedName(){return label129(selected);}
    void selectChoice(int position){select(MenuTouchSettings.editorIndex129(position));}
    private float dp(float n){return PortUi.dp(getContext(),n);}
    void setChanged(Runnable value){changed=value;}
    int selected(){return selected;}
    void select(int index){if(!MenuTouchSettings.editable129(index))return;selected=index;if(changed!=null)changed.run();invalidate();}
    void appearanceChanged(){invalidate();if(changed!=null)changed.run();}
    private void geometry(){
        float w=getWidth(),h=getHeight();
        if(w<=0||h<=0)return;
        for(int i=0;i<boxes.length;i++){
            float hw,hh;
            if(!MenuTouchSettings.editable129(i)){boxes[i].setEmpty();continue;}
            if(i==MenuTouchSettings.DPAD)continue;
            if(i>=MenuTouchSettings.LT && !(settings.customArtwork && artwork[i] != null)){hw=Math.min(h*.068f,w*.04f)*1.1f*settings.size[i];hh=hw*.65f/1.1f;}
            else if(i<2||i>=MenuTouchSettings.X)hw=hh=Math.min(h*.068f,w*.04f)*settings.size[i];
            else{hw=Math.min(w/2,dp(40*settings.size[i]));hh=Math.min(h/2,dp(18*settings.size[i]));}
            float x=MenuTouchSettings.center(settings.x[i],w,hw),y=MenuTouchSettings.center(settings.y[i],h,hh);
            boxes[i].set(x-hw,y-hh,x+hw,y+hh);
        }
        pad.x=settings.x[2];pad.y=settings.y[2];pad.scale=settings.size[2];pad.layout(w,h,h*.075f);
        boxes[2].set(pad.left,pad.top,pad.right,pad.bottom);
    }
    private void label(Canvas c,String value,RectF r,float size){
        paint.setStyle(Paint.Style.FILL);paint.setTextSize(size);paint.setTextAlign(Paint.Align.CENTER);
        paint.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);paint.setColor(PortUi.TEXT);
        c.drawText(value,r.centerX(),r.centerY()-(paint.ascent()+paint.descent())/2,paint);
    }
    protected void onDraw(Canvas c){
        geometry();
        if(owner!=-1)editorGuide.draw(c,getWidth(),getHeight(),boxes[selected].centerX(),boxes[selected].centerY());
        for(int i=0;i<boxes.length;i++){
            if(!MenuTouchSettings.editable129(i))continue;
            RectF r=boxes[i];int alpha=Math.round(255*Math.max(.15f,settings.alpha[i]));
            paint.setStyle(Paint.Style.FILL);paint.setColor((alpha<<24)|0x111822);
            boolean face=i==MenuTouchSettings.A||i==MenuTouchSettings.B||i==MenuTouchSettings.X||i==MenuTouchSettings.Y;
            if (settings.customArtwork && artwork[i] != null) {
                artworkPaint.setAlpha(alpha);
                c.drawBitmap(artwork[i], null, r, artworkPaint);
            } else if(face)c.drawOval(r,paint);else c.drawRoundRect(r,dp(10),dp(10),paint);
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(dp(i==selected?3:1));paint.setColor(i==selected?0xffffd48c:PortUi.LINE);
            if(face)c.drawOval(r,paint);else c.drawRoundRect(r,dp(10),dp(10),paint);
            if(i==MenuTouchSettings.DPAD){
                for(int j=0;j<4;j++){
                    float half=pad.cell*.48f;arrow.set(pad.cx[j]-half,pad.cy[j]-half,pad.cx[j]+half,pad.cy[j]+half);
                    if (settings.customArtwork && arrows[j] != null) {
                        artworkPaint.setAlpha(alpha);
                        c.drawBitmap(arrows[j], null, arrow, artworkPaint);
                    } else {
                        label(c,ARROWS[j],arrow,pad.cell*.5f);
                    }
                }
            }else label(c,label129(i),r,Math.min(dp(14)*settings.size[i],r.height()*.4f));
        }
    }
    @Override public boolean onTouchEvent(MotionEvent event){
        int action=event.getActionMasked();
        if(action==MotionEvent.ACTION_DOWN){
            geometry();int hit=-1;
            for(int i=boxes.length-1;i>=0;i--)if(MenuTouchSettings.editable129(i)&&boxes[i].contains(event.getX(),event.getY())){hit=i;break;}
            if(hit<0){owner=-1;return true;}
            owner=event.getPointerId(0);select(hit);
            dx=boxes[hit].centerX()-event.getX();dy=boxes[hit].centerY()-event.getY();
            return true;
        }
        int pointer=event.findPointerIndex(owner);
        if(action==MotionEvent.ACTION_MOVE&&pointer>=0&&getWidth()>0&&getHeight()>0){
            RectF r=boxes[selected];
            settings.x[selected]=MenuTouchSettings.center((event.getX(pointer)+dx)/getWidth(),getWidth(),r.width()/2)/getWidth();
            settings.y[selected]=MenuTouchSettings.center((event.getY(pointer)+dy)/getHeight(),getHeight(),r.height()/2)/getHeight();
            invalidate();
        }
        if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL||
                (action==MotionEvent.ACTION_POINTER_UP&&event.getPointerId(event.getActionIndex())==owner)){
            owner=-1;invalidate();
        }
        return true;
    }
}
