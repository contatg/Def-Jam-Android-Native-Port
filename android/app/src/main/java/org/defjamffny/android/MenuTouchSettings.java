package org.defjamffny.android;

import android.content.Context;
import android.util.AtomicFile;
import java.io.File;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;
import org.json.JSONException;

/** Menu controls; retired shortcut indices remain reserved for saved-layout compatibility. */
final class MenuTouchSettings {
    // Append indices so existing A/B/D-pad/start/online layouts remain intact.
    static final int A=0,B=1,DPAD=2,START=3,ONLINE=4,X=5,Y=6,LT=7,RT=8,LB=9,RB=10,COUNT=11;
    private static final int[] EDITOR129={A,B,DPAD,START,X,Y,LT,RT,LB,RB};
    static boolean editable129(int index){return index>=0&&index<COUNT&&index!=ONLINE;}
    static int editorCount129(){return EDITOR129.length;}
    static int editorIndex129(int position){return EDITOR129[position];}
    private static final float[] DEFAULT_X={.86f,.95f,.14f,.44f,.58f,.86f,.95f,.86f,.95f,.86f,.95f};
    private static final float[] DEFAULT_Y={.83f,.83f,.76f,.94f,.94f,.65f,.65f,.47f,.47f,.34f,.34f};
    final float[] size=new float[COUNT], alpha=new float[COUNT];
    boolean playStation;
    void setStyle127(int style){playStation=style==FightControlProfiles.PLAYSTATION;}
    final float[] x=DEFAULT_X.clone(),y=DEFAULT_Y.clone();
    MenuTouchSettings(JSONObject data) {
        reset();
        setStyle127(data==null?0:data.optInt("fight_style122",0));
        if(data!=null) for(int i=0;i<COUNT;i++) {
            size[i]=bounded((float)data.optDouble("menu_"+i+"_size",1),.5f,2.2f);
            alpha[i]=bounded((float)data.optDouble("menu_"+i+"_alpha",1),i==ONLINE?.25f:0,1);
            x[i]=bounded((float)data.optDouble("menu_"+i+"_x",DEFAULT_X[i]),0,1);
            y[i]=bounded((float)data.optDouble("menu_"+i+"_y",DEFAULT_Y[i]),0,1);
        }
    }
    private static float bounded(float value,float min,float max) {
        return Float.isNaN(value)||Float.isInfinite(value)?min:Math.max(min,Math.min(max,value));
    }
    static float center(float fraction,float extent,float half) {
        return Math.max(Math.min(half,extent/2),Math.min(extent-half,fraction*extent));
    }
    void reset() {
        java.util.Arrays.fill(size,1); java.util.Arrays.fill(alpha,1);
        System.arraycopy(DEFAULT_X,0,x,0,COUNT);System.arraycopy(DEFAULT_Y,0,y,0,COUNT);
    }
    void write(JSONObject data) throws JSONException {
        for(int i=0;i<COUNT;i++) {
            data.put("menu_"+i+"_size",size[i]); data.put("menu_"+i+"_alpha",alpha[i]);
            data.put("menu_"+i+"_x",x[i]); data.put("menu_"+i+"_y",y[i]);
        }
    }
    static MenuTouchSettings read(Context context) {
        try {
            AtomicFile file=new AtomicFile(new File(context.getFilesDir(),"controls-v7.json"));
            return new MenuTouchSettings(new JSONObject(new String(file.readFully(),StandardCharsets.UTF_8)));
        } catch(Exception error) { return new MenuTouchSettings(null); }
    }
}
