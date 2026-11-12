package org.defjamffny.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.ProgressDialog;
import android.net.Uri;
import android.os.Environment;
import android.os.SystemClock;
import android.widget.Toast;
import java.io.File;
import java.io.IOException;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;

/** Copies the extracted game to a private XISO for the embedded hakuX core. */
final class HakuXImageBuilder34 {
    private static String tr(String en,String pt,String es){return UiText.isPortuguese()?pt:UiText.isSpanish()?es:en;}
    private static void toast(Activity a,String s){Toast.makeText(a,s,Toast.LENGTH_LONG).show();}
    private static final AtomicBoolean BUILDING = new AtomicBoolean();
    static void prepare(Activity a){
        File root=new File(Environment.getExternalStorageDirectory(),"DefJamFFNY/game");
        if(!new File(root,"default.xbe").isFile()){
            toast(a,tr("Extract the game first.","Extraia o jogo primeiro.","Extrae el juego primero."));
            return;
        }
        File ext=a.getExternalFilesDir(null);if(ext==null){toast(a,"Armazenamento indisponível.");return;}
        File dir=XisoShareProvider34.directory(a);if(!dir.isDirectory()&&!dir.mkdirs()){toast(a,"Não foi possível criar a pasta temporária.");return;}
        if(!BUILDING.compareAndSet(false,true))return;
        File part=new File(dir,"DefJam_"+UUID.randomUUID().toString().replace("-","")+".part");
        AtomicBoolean cancelled=new AtomicBoolean();ProgressDialog progress=new ProgressDialog(a,android.R.style.Theme_DeviceDefault_Dialog_Alert);
        progress.setTitle(tr("Creating Xbox image","Criando imagem Xbox","Creando imagen Xbox"));progress.setMessage(tr("Copying game files…","Copiando arquivos do jogo…","Copiando archivos del juego…"));progress.setProgressStyle(ProgressDialog.STYLE_HORIZONTAL);progress.setMax(1000);progress.setCancelable(true);progress.setCanceledOnTouchOutside(false);progress.setOnCancelListener(d->cancelled.set(true));progress.show();
        new Thread(()->{
            try{
                String name="DefJam_"+new XisoWriter34(root,null).sourceStamp();
                File ready=new File(dir,name+".xiso.iso");
                final long[] last={0};
                if(!ready.isFile()) {
                new XisoWriter34(root,(done,total)->{
                if(cancelled.get())throw new IOException("Cancelado");long now=SystemClock.elapsedRealtime();
                if(now-last[0]>150||done==total){last[0]=now;int value=total<=0?0:(int)Math.min(1000,done*1000.0/total);a.runOnUiThread(()->{if(!a.isFinishing()&&!a.isDestroyed())progress.setProgress(value);});}
            }).write(part);
                if(cancelled.get())throw new IOException("Cancelado");
                if(!part.renameTo(ready))throw new IOException("Não foi possível finalizar a XISO.");
                }
                if(cancelled.get())return;
                Uri uri=XisoShareProvider34.uri(a,ready);
                a.runOnUiThread(()->{if(!a.isFinishing()&&!a.isDestroyed()){progress.dismiss();HakuxLauncher34.launch(a,uri);}});
            }catch(Exception e){part.delete();a.runOnUiThread(()->{if(!a.isFinishing()&&!a.isDestroyed()){progress.dismiss();if(!cancelled.get())new AlertDialog.Builder(a,android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle("hakuX").setMessage(e.getMessage()).setPositiveButton("OK",null).show();}});}
            finally{BUILDING.set(false);}
        },"defjam-xiso34").start();
    }
    private HakuXImageBuilder34(){}
}
