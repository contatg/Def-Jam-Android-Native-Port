package org.defjamffny.android;

import android.content.Context;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;

/** Independent custom, Xbox and PlayStation fight layouts; original menu/online fields remain at the root. */
final class FightControlProfiles {
    static final int CUSTOM=0,XBOX=1,PLAYSTATION=2;
    private static int valid(int value){return value==XBOX||value==PLAYSTATION?value:CUSTOM;}
    private String profileKey(){return style==PLAYSTATION?"fight_playstation127":style==XBOX?"fight_xbox122":"fight_custom122";}
    private final JSONObject root;
    private int style;
    private static JSONObject copy(JSONObject value){
        try{return value==null?new JSONObject():new JSONObject(value.toString());}
        catch(Exception error){throw new IllegalArgumentException("Invalid control document",error);}
    }
    FightControlProfiles(JSONObject value){root=copy(value);style=valid(root.optInt("fight_style122",CUSTOM));}
    int style(){return style;}
    void setStyle(int value){style=valid(value);}
    JSONObject active(){
        JSONObject saved=root.optJSONObject(profileKey());
        if(saved!=null)return copy(saved);
        if(style!=CUSTOM)return null;
        boolean hasLegacy=root.has("controls_v7_arrows")||root.has("dpad_v8");
        java.util.Iterator<String> keys=root.keys();while(keys.hasNext())hasLegacy|=keys.next().startsWith("c6_");
        if(!hasLegacy)return null;
        // First custom profile inherits the complete old geometry and remaps.
        JSONObject legacy=copy(root);legacy.remove("fight_xbox122");legacy.remove("fight_custom122");legacy.remove("fight_playstation127");
        return legacy.length()==0?null:legacy;
    }
    void store(JSONObject values){
        try{
            root.put(profileKey(),copy(values));
            if(style==CUSTOM){
                java.util.Iterator<String> keys=values.keys();
                while(keys.hasNext()){
                    String key=keys.next();
                    if(key.startsWith("c6_")||key.startsWith("dpad_")||key.equals("controls_v7_arrows")||key.equals("fight_keys_v15")||key.equals("run_special_v16"))root.put(key,values.get(key));
                }
            }
        }
        catch(Exception error){throw new IllegalStateException(error);}
    }
    JSONObject document(){
        JSONObject value=copy(root);
        try{value.put("fight_style122",style);}catch(Exception error){throw new IllegalStateException(error);}
        return value;
    }
    static JSONObject read(Context context){
        try{return new JSONObject(new String(file(context).readFully(),StandardCharsets.UTF_8));}
        catch(java.io.FileNotFoundException missing){return null;}
        catch(Exception error){throw new IllegalStateException("Cannot read controls; saved layouts were not changed",error);}
    }
    static boolean choose(Context context,int value){
        try{FightControlProfiles profiles=new FightControlProfiles(read(context));profiles.setStyle(value);return write(context,profiles.document());}
        catch(Exception error){android.util.Log.e("DefJamControls","Cannot switch fight profile",error);return false;}
    }
    private static AtomicFile file(Context context){return new AtomicFile(new File(context.getFilesDir(),"controls-v7.json"));}
    static boolean write(Context context,JSONObject document){
        AtomicFile file=file(context);FileOutputStream stream=null;
        try{stream=file.startWrite();stream.write(document.toString().getBytes(StandardCharsets.UTF_8));file.finishWrite(stream);return true;}
        catch(Exception error){if(stream!=null)file.failWrite(stream);android.util.Log.e("DefJamControls","Cannot save control profiles",error);return false;}
    }
}
