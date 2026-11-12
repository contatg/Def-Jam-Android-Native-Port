package org.defjamffny.android;

import android.app.Activity;
import android.os.Build;
import android.os.PowerManager;
import android.util.Log;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;

/* Window/surface requests only. No controls, layout, game timing or audio edits. */
final class PowerPolicy25 {
    private final Activity activity;
    private boolean sustained;
    private int savedMode = -1, lastHz = -1;
    private Surface lastSurface;
    PowerPolicy25(Activity a) { activity=a; }
    private Surface findSurface(View v) {
        if (v instanceof SurfaceView) {
            Surface s=((SurfaceView)v).getHolder().getSurface();
            if(s!=null&&s.isValid())return s;
        }
        if(v instanceof ViewGroup){ViewGroup g=(ViewGroup)v;for(int i=0;i<g.getChildCount();i++){Surface s=findSurface(g.getChildAt(i));if(s!=null)return s;}}
        return null;
    }
    private void record(int hz){Display d=activity.getWindow().getDecorView().getDisplay();DefJamActivity.nativePowerDisplay25(hz,d==null?0:Math.round(d.getRefreshRate()*1000f),Build.VERSION.SDK_INT>=23&&d!=null?d.getMode().getModeId():0,sustained?1:0);}
    void apply(int bits) {
        if((bits&(1<<20))==0)return;
        boolean wantSustained=(bits&1)!=0;
        if(Build.VERSION.SDK_INT>=24&&wantSustained!=sustained){
            PowerManager pm=(PowerManager)activity.getSystemService(Activity.POWER_SERVICE);
            if(!wantSustained||(pm!=null&&pm.isSustainedPerformanceModeSupported())){
                activity.getWindow().setSustainedPerformanceMode(wantSustained);sustained=wantSustained;
                Log.i("POWER25","sustained request="+sustained+"; clocks remain OS controlled");
            }else if(lastHz==-1)Log.i("POWER25","sustained performance unsupported");
        }
        int cap=(bits>>>8)&255;
        int hz=(bits&2)==0?0:(bits&(1<<21))!=0?(cap==60||cap==90||cap==120?cap:60):cap==40?120:cap==60||cap==30?60:0;
        Surface s=findSurface(activity.getWindow().getDecorView());
        if(hz==lastHz&&s==lastSurface){record(hz);return;}
        if(savedMode<0)savedMode=activity.getWindow().getAttributes().preferredDisplayModeId;
        Display display=activity.getWindow().getDecorView().getDisplay();
        int selected=0;
        if(Build.VERSION.SDK_INT>=23&&display!=null&&hz>0){
            Display.Mode current=display.getMode();
            for(Display.Mode mode:display.getSupportedModes())if(mode.getPhysicalWidth()==current.getPhysicalWidth()&&mode.getPhysicalHeight()==current.getPhysicalHeight()&&Math.abs(mode.getRefreshRate()-hz)<1.0f){selected=mode.getModeId();break;}
        }
        WindowManager.LayoutParams lp=activity.getWindow().getAttributes();
        lp.preferredDisplayModeId=selected!=0?selected:savedMode;
        activity.getWindow().setAttributes(lp);
        if(Build.VERSION.SDK_INT>=30&&s!=null){
            float sourceRate=hz>0?(float)cap:0f;
            try {
                if(Build.VERSION.SDK_INT>=31)s.setFrameRate(sourceRate,Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,Surface.CHANGE_FRAME_RATE_ALWAYS);
                else s.setFrameRate(sourceRate,Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
            }catch(RuntimeException error){Log.w("POWER25","Surface frame-rate request unavailable",error);}
        }
        lastHz=hz;lastSurface=s;record(hz);
        Log.i("POWER25","display_hz_request="+hz+" source_fps_request="+cap+" mode_id="+selected+" observed_hz="+(display==null?-1:display.getRefreshRate())+"; request may be overridden by Android");
    }
    void reset() {
        if(Build.VERSION.SDK_INT>=30&&lastSurface!=null&&lastSurface.isValid())try{lastSurface.setFrameRate(0f,Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);}catch(RuntimeException ignored){}
        if(savedMode>=0){WindowManager.LayoutParams lp=activity.getWindow().getAttributes();lp.preferredDisplayModeId=savedMode;activity.getWindow().setAttributes(lp);}
        if(Build.VERSION.SDK_INT>=24&&sustained)activity.getWindow().setSustainedPerformanceMode(false);
        sustained=false;savedMode=-1;lastHz=-1;lastSurface=null;
    }
}
