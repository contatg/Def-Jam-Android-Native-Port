// SPDX-License-Identifier: GPL-3.0-or-later
package org.defjamffny.android;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;
import android.widget.Toast;

/** Starts the embedded hakuX core using the user's ISO or generated XISO. */
final class HakuxLauncher34 {
    private static String tr(String en,String pt,String es){return UiText.isPortuguese()?pt:UiText.isSpanish()?es:en;}
    static boolean running(Activity a){
        ActivityManager am=(ActivityManager)a.getSystemService(Activity.ACTIVITY_SERVICE);
        if(am==null)return false;java.util.List<ActivityManager.RunningAppProcessInfo> ps=am.getRunningAppProcesses();
        if(ps!=null)for(ActivityManager.RunningAppProcessInfo p:ps)if((a.getPackageName()+":hakux").equals(p.processName))return true;
        return false;
    }
    private static boolean canStart(Activity a){
        if(!running(a))return true;
        Toast.makeText(a,tr("Close the emulated game first.","Feche a emulação aberta primeiro.","Cierra primero el juego emulado."),Toast.LENGTH_LONG).show();return false;
    }
    static void open(Activity a){
        if(canStart(a))HakuXImageBuilder34.prepare(a);
    }
    static void launch(Activity a,Uri uri){
        if(!canStart(a))return;
        try{
            try(android.os.ParcelFileDescriptor file=a.getContentResolver().openFileDescriptor(uri,"r")){
                if(file==null)throw new java.io.IOException("Image unavailable");}
            Intent play=new Intent(a,HakuXGame36.class).putExtra(HakuXGame36.IMAGE,uri.toString())
                .setData(uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            play.setClipData(ClipData.newRawUri("Xbox ISO",uri));HakuXOptions36.putExtras(a,play);a.startActivity(play);
        }catch(ActivityNotFoundException|SecurityException|java.io.IOException e){
            Toast.makeText(a,tr("Could not open this image; choose it again.","Não foi possível abrir a imagem preparada.","No se pudo abrir la imagen; vuelve a elegirla."),Toast.LENGTH_LONG).show();
        }
    }
    private HakuxLauncher34(){}
}
