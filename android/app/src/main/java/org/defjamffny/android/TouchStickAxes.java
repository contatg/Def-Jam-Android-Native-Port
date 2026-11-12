package org.defjamffny.android;

/** Circular touch travel -> real Xbox signed axes. Android Y points down. */
final class TouchStickAxes {
    private final int[] axes=new int[4];
    int get(int axis){return axes[axis];}
    int get(int axis,boolean blazin){
        if(blazin&&axis==3&&axes[2]==0&&axes[3]==0)return 32767;
        return axes[axis];
    }
    void center(int stick){axes[stick*2]=axes[stick*2+1]=0;}
    void clear(){java.util.Arrays.fill(axes,0);}
    void move(int stick,float x,float y,float deadzone){
        if(stick<0||stick>1)throw new IllegalArgumentException("stick");
        if(!Float.isFinite(x)||!Float.isFinite(y)){center(stick);return;}
        double d=Float.isFinite(deadzone)?Math.max(0,Math.min(.95,deadzone)):.15;
        double length=Math.hypot(x,y);
        if(length<=d||length==0){center(stick);return;}
        double magnitude=(Math.min(1,length)-d)/(1-d);
        axes[stick*2]=(int)Math.round(x/length*magnitude*32767);
        axes[stick*2+1]=(int)Math.round(-y/length*magnitude*32767);
    }
}
