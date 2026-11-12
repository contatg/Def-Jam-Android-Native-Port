package org.defjamffny.android;

import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;

/** APK-only recovery entry. Never visible in fights, native lobbies or patched menus. */
final class OnlineMenuEntry137 {
    private final DefJamActivity activity;
    private final Button button;
    private long menuSince=-1;
    private boolean visible,logged;
    private int lastWidth,lastHeight;
    OnlineMenuEntry137(DefJamActivity activity){
        this.activity=activity;button=PortUi.button(activity,"ONLINE",true);
        button.setContentDescription("ONLINE");button.setVisibility(View.GONE);
        FrameLayout.LayoutParams p=new FrameLayout.LayoutParams(PortUi.dp(activity,144),PortUi.dp(activity,48),Gravity.TOP|Gravity.LEFT);
        p.leftMargin=PortUi.dp(activity,24);p.topMargin=PortUi.dp(activity,100);activity.addContentView(button,p);
        button.setOnClickListener(v->{hide();activity.openNativeOnline();});
    }
    static boolean wanted(int flags,boolean live,boolean focus,long since,long now){
        return (flags&1)!=0&&(flags&2)==0&&!live&&focus&&since>=0&&now-since>=1800;
    }
    void update(int flags,boolean live,boolean focus,long now){
        if((flags&1)==0){menuSince=-1;hide();return;}
        if(menuSince<0)menuSince=now;
        boolean show=wanted(flags,live,focus,menuSince,now);
        if(show){
            FrameLayout.LayoutParams p=(FrameLayout.LayoutParams)button.getLayoutParams();
            int w=activity.getResources().getDisplayMetrics().widthPixels,h=activity.getResources().getDisplayMetrics().heightPixels;
            if(w!=lastWidth||h!=lastHeight){lastWidth=w;lastHeight=h;p.leftMargin=Math.round(w*.12f);p.topMargin=Math.round(h*.56f);button.setLayoutParams(p);}
            if(!visible){visible=true;button.setVisibility(View.VISIBLE);button.bringToFront();}
            if(!logged){logged=true;android.util.Log.i("ONLINE-MENU137","APK recovery entry active: main menu has no Online.Tick; no PC override required");}
        }else hide();
    }
    void hide(){visible=false;button.setVisibility(View.GONE);}
    void destroy(){hide();if(button.getParent() instanceof android.view.ViewGroup)((android.view.ViewGroup)button.getParent()).removeView(button);}
}
