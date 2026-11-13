package org.defjamffny.android;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.os.Build;
import android.os.Environment;
import android.widget.Toast;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Comparator;
import java.util.concurrent.atomic.AtomicBoolean;

/** Local crash reports for the port's own process; nothing is uploaded. */
final class CrashReport35 {
    private static final AtomicBoolean installed=new AtomicBoolean(),collecting=new AtomicBoolean();
    private static String tr(String en,String pt,String es){return UiText.isPortuguese()?pt:UiText.isSpanish()?es:en;}
    private static File dir(Context c){
        File d=new File(Environment.getExternalStorageDirectory(),"DefJamFFNY/logs");
        if(d.isDirectory()||d.mkdirs())return d;
        File base=c.getExternalFilesDir(null);if(base==null)base=c.getFilesDir();
        d=new File(base,"logs");if(!d.isDirectory())d.mkdirs();return d;
    }
    static void install(Context c){
        if(!installed.compareAndSet(false,true))return;
        Context app=c.getApplicationContext();Thread.UncaughtExceptionHandler prior=Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread,error)->{
            try{File out=new File(dir(app),"java-crash-"+System.currentTimeMillis()+".txt");
                try(FileOutputStream stream=new FileOutputStream(out);PrintWriter p=new PrintWriter(new OutputStreamWriter(stream,StandardCharsets.UTF_8))){
                    p.println("Def Jam Android — Java crash");p.println("Device: "+Build.MANUFACTURER+" "+Build.MODEL+" / API "+Build.VERSION.SDK_INT);p.println("Thread: "+thread.getName());error.printStackTrace(p);p.flush();stream.getFD().sync();}
            }catch(Throwable ignored){}
            if(prior!=null)prior.uncaughtException(thread,error);else android.os.Process.killProcess(android.os.Process.myPid());
        });
    }
    static void collect(Activity a){
        if(Build.VERSION.SDK_INT<30||!collecting.compareAndSet(false,true))return;
        Context app=a.getApplicationContext();
        new Thread(()->{try{boolean found=Api30.collect(app);if(found)a.runOnUiThread(()->{
            if(!a.isFinishing()&&!a.isDestroyed())Toast.makeText(a,tr("Crash report saved. Open Error reports in the launcher.","Relatório de erro salvo. Abra Relatórios de erro no launcher.","Informe de error guardado. Abre Informes de error en el launcher."),Toast.LENGTH_LONG).show();
        });}finally{collecting.set(false);}},"defjam-crash-report").start();
    }
    @android.annotation.TargetApi(30)
    private static final class Api30 {
        static boolean abnormal(int reason){return reason==android.app.ApplicationExitInfo.REASON_CRASH||reason==android.app.ApplicationExitInfo.REASON_CRASH_NATIVE||
            reason==android.app.ApplicationExitInfo.REASON_ANR||reason==android.app.ApplicationExitInfo.REASON_INITIALIZATION_FAILURE||
            reason==android.app.ApplicationExitInfo.REASON_LOW_MEMORY||reason==android.app.ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE||
            reason==android.app.ApplicationExitInfo.REASON_SIGNALED;}
        static boolean collect(Context c){
            try{ActivityManager m=(ActivityManager)c.getSystemService(Context.ACTIVITY_SERVICE);if(m==null)return false;
                java.util.List<android.app.ApplicationExitInfo> entries=m.getHistoricalProcessExitReasons(c.getPackageName(),0,20);
                android.content.SharedPreferences prefs=c.getSharedPreferences("diagnostic35",Context.MODE_PRIVATE);long last=prefs.getLong("last_exit",0),latest=last;boolean saved=false;
                for(android.app.ApplicationExitInfo e:entries){String name=e.getProcessName();long stamp=e.getTimestamp();
                    if(name==null||!name.equals(c.getPackageName()+":game")||stamp<=last||!abnormal(e.getReason()))continue;
                    File folder=dir(c),report=new File(folder,"game-exit-"+stamp+".txt"),trace=new File(folder,"game-exit-"+stamp+".trace");
                    try(FileOutputStream stream=new FileOutputStream(report);PrintWriter p=new PrintWriter(new OutputStreamWriter(stream,StandardCharsets.UTF_8))){
                        p.println("Def Jam Android — previous game-process exit");p.println("Timestamp (ms): "+stamp);p.println("Process: "+name);p.println("Device: "+Build.MANUFACTURER+" "+Build.MODEL+" / API "+Build.VERSION.SDK_INT);
                        p.println("Reason: "+e.getReason()+" / status: "+e.getStatus()+" / importance: "+e.getImportance());p.println("Description: "+e.getDescription());p.println("If available, a trace is beside this file. Also include defjam-native.log.");p.flush();stream.getFD().sync();}
                    try(InputStream in=e.getTraceInputStream()){
                        if(in!=null)try(FileOutputStream out=new FileOutputStream(trace)){byte[] b=new byte[8192];int n,left=262144;while(left>0&&(n=in.read(b,0,Math.min(left,b.length)))>0){out.write(b,0,n);left-=n;}}
                    }catch(IOException|RuntimeException ignored){}
                    latest=Math.max(latest,stamp);saved=true;
                }
                if(latest>last)prefs.edit().putLong("last_exit",latest).apply();return saved;
            }catch(RuntimeException|IOException ex){android.util.Log.w("DefJamReport35","Could not collect process exit information",ex);return false;}
        }
    }
    static void show(Activity a){
        File folder=dir(a);File[] files=folder.listFiles((d,n)->(n.startsWith("game-exit-")||n.startsWith("java-crash-"))&&n.endsWith(".txt"));
        String message=tr("No crash report was found yet.","Ainda não foi encontrado relatório de erro.","Todavía no se encontró un informe de error.");
        if(files!=null&&files.length>0){Arrays.sort(files,Comparator.comparingLong(File::lastModified).reversed());File last=files[0];
            try(FileInputStream in=new FileInputStream(last)){byte[] data=new byte[4096];int n=in.read(data);message=(n>0?new String(data,0,n,StandardCharsets.UTF_8):"")+"\n"+last.getAbsolutePath();}
            catch(IOException e){message=last.getAbsolutePath();}}
        message+="\n\n"+tr("Also send defjam-native.log when asking for support. Reports remain on this device.","Envie também o defjam-native.log ao pedir suporte. Os relatórios ficam neste celular.","Envía también defjam-native.log al pedir soporte. Los informes permanecen en este dispositivo.");
        PortUi.show(PortUi.dialog(a).setTitle(tr("Error reports","Relatórios de erro","Informes de error")).setMessage(message).setPositiveButton("OK",null));
    }
    private CrashReport35(){}
}
