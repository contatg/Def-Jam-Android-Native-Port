package org.defjamffny.android;

/** One grouped pad for drawing, hit testing, moving and scaling. No Android dependencies. */
final class TouchDpadLayout {
    float x=.15f, y=.75f, scale=1f, cell, left, top, right, bottom;
    boolean visible=true, positioned;
    final float[] cx=new float[4], cy=new float[4]; // up, down, left, right
    private float centerX, centerY;
    private static float clamp(float v,float lo,float hi) { return Math.max(lo,Math.min(hi,v)); }
    void layout(float width,float height,float baseCell) {
        cell=Math.max(1f,Math.min(baseCell*scale,Math.min(width,height)/3f));
        float half=cell*1.5f;
        centerX=clamp(x*width,half,Math.max(half,width-half));
        centerY=clamp(y*height,half,Math.max(half,height-half));
        left=centerX-half; right=centerX+half; top=centerY-half; bottom=centerY+half;
        cx[0]=cx[1]=centerX; cy[0]=centerY-cell; cy[1]=centerY+cell;
        cy[2]=cy[3]=centerY; cx[2]=centerX-cell; cx[3]=centerX+cell;
    }
    boolean contains(float px,float py) { return px>=left&&px<=right&&py>=top&&py<=bottom; }
    int directionsAt(float px,float py) {
        if(!contains(px,py)) return 0;
        float dx=px-centerX, dy=py-centerY, dead=cell*.30f;
        int bits=0;
        if(dy < -dead) bits|=1; else if(dy > dead) bits|=2;
        if(dx < -dead) bits|=4; else if(dx > dead) bits|=8;
        return bits;
    }
}
