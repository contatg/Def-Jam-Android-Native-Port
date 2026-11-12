// SPDX-License-Identifier: GPL-3.0-or-later
package org.defjamffny.android;

import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.RelativeLayout;
import android.widget.TextView;
import com.rfandango.haku_x.NativeCoreActivity;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.TimeZone;
import org.libsdl.hak.SDLActivity;

/** Embedded hakuX, isolated in :hakux with its own SDL3 and data directory. */
public final class HakuXGame36 extends SDLActivity {
    static final String IMAGE="hakux36_uri", RELAUNCH="hakux36_relaunch";
    private final Handler handler=new Handler(Looper.getMainLooper());
    private final Runnable poll=this::pollCore;
    private String disc="fd:-1",openError="";
    private volatile String driverError="";
    private volatile String attemptedLibrary="";
    private boolean ready,active,moving,showFps;
    private HakuXConfig36 config;
    private HakuXControls36 controls;
    private TextView loading,fps;
    private AlertDialog menu;
    private SharedPreferences runtime;
    private String tr(String en,String pt,String es){return UiText.isPortuguese()?pt:UiText.isSpanish()?es:en;}

    @Override protected void onCreate(Bundle state){
        Intent in=getIntent();config=HakuXOptions36.config(in);
        UiText.setLanguage(getSharedPreferences("launcher",MODE_PRIVATE).getString("ui_language","en"));
        try{
            String uri=in.getStringExtra(IMAGE);if(uri==null)throw new java.io.IOException("ISO ausente");
            ParcelFileDescriptor fd=getContentResolver().openFileDescriptor(Uri.parse(uri),"r");
            if(fd==null)throw new java.io.IOException("ISO indisponível");disc="fd:"+fd.detachFd();
        }catch(Exception ex){openError=String.valueOf(ex.getMessage());}
        super.onCreate(state);
        if(mBrokenLibraries||mLayout==null)return;
        ready=true;getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        runtime=getSharedPreferences("hakux36_runtime",MODE_PRIVATE);controls=new HakuXControls36(this,this::openMenu);
        controls.setShown(config.touch);controls.setStick(runtime.getBoolean("analog",true));controls.setRightStick(runtime.getBoolean("right_stick",true));controls.setOpacity(runtime.getInt("opacity",0));
        mLayout.addView(controls,new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,ViewGroup.LayoutParams.MATCH_PARENT));
        loading=overlay(tr("Starting emulator…","Iniciando emulador…","Iniciando emulador…"),RelativeLayout.CENTER_IN_PARENT);
        fps=overlay("",RelativeLayout.ALIGN_PARENT_TOP);fps.setTextColor(0xffe5e9ed);showFps=config.fps;fps.setVisibility(View.GONE);
        if(!openError.isEmpty())showFailure(tr("Could not open the image.","Não foi possível abrir a imagem.","No se pudo abrir la imagen.")+"\n"+openError);
    }
    private TextView overlay(String text,int rule){
        TextView view=new TextView(this);view.setText(text);view.setTextColor(0xffffdf9b);view.setTextSize(15);
        view.setPadding(18,8,18,8);view.setBackgroundColor(0xcc10151c);RelativeLayout.LayoutParams lp=new RelativeLayout.LayoutParams(-2,-2);
        lp.addRule(rule);mLayout.addView(view,lp);return view;
    }
    @Override protected String[] getLibraries(){return new String[]{"hSD3","hakuxcore"};}
    @Override protected String[] getArguments(){
        File hdd=new File(getFilesDir(),"hakux36/disk");hdd.mkdirs();String driver="";
        String library=getIntent().getStringExtra("hakux36_driver_library");
        if(library!=null&&!library.isEmpty())try{
            driver=HakuXDriver36.specification(this,library);
            if(getIntent().getBooleanExtra("hakux36_a610_compat",false)){
                String flags=android.system.Os.getenv("TU_DEBUG");
                if(flags==null||flags.isEmpty())flags="noconform";
                else if(!java.util.Arrays.asList(flags.split(",")).contains("noconform"))flags+=",noconform";
                android.system.Os.setenv("TU_DEBUG",flags,true);
            }
        }
        catch(Exception ex){driver="";driverError=String.valueOf(ex.getMessage());android.util.Log.w("HAKUX36","Turnip unavailable; system driver selected",ex);
            handler.post(()->{if(!isFinishing())android.widget.Toast.makeText(this,tr("Turnip could not be prepared; using the system driver.","Não foi possível preparar o Turnip; usando o driver do sistema.","No se pudo preparar Turnip; se usará el controlador del sistema."),android.widget.Toast.LENGTH_LONG).show();});}
        attemptedLibrary = driver.isEmpty() ? "" : library;
        List<String> args=config.arguments(disc,hdd.getAbsolutePath(),
            TimeZone.getDefault().getOffset(System.currentTimeMillis())/60000,driver,getIntent().getBooleanExtra(RELAUNCH,false));
        return args.toArray(new String[0]);
    }
    @Override protected boolean keepFullscreen(){return true;}
    @Override public void onWindowFocusChanged(boolean focused){super.onWindowFocusChanged(focused);if(focused)hideBars();}
    private void hideBars(){
        if(Build.VERSION.SDK_INT>=30){WindowInsetsController c=getWindow().getInsetsController();if(c!=null){c.hide(WindowInsets.Type.systemBars());c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);}}
        else getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN|View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }
    @Override protected void onResume(){super.onResume();active=true;syncPause();hideBars();if(ready&&!moving)handler.post(poll);}
    @Override protected void onPause(){active=false;handler.removeCallbacks(poll);if(controls!=null)controls.reset();syncPause();super.onPause();}
    @Override protected void onDestroy(){moving=true;handler.removeCallbacks(poll);if(menu!=null)menu.dismiss();super.onDestroy();
        if(isFinishing()&&!isChangingConfigurations())android.os.Process.killProcess(android.os.Process.myPid());}
    @Override public boolean dispatchKeyEvent(KeyEvent event){
        if(event.getKeyCode()==KeyEvent.KEYCODE_BACK&&!event.isFromSource(android.view.InputDevice.SOURCE_GAMEPAD)){
            if(event.getAction()==KeyEvent.ACTION_UP)onBackPressed();return true;}return super.dispatchKeyEvent(event);
    }
    @Override public void onBackPressed(){if(ready&&!moving)openMenu();else finish();}
    private void syncPause(){if(ready)try{NativeCoreActivity.nativeSetPaused(!active||menu!=null||moving);}catch(LinkageError ignored){}}
    private void pollCore(){
        if(!active||moving)return;
        try{
            String fatal=NativeCoreActivity.nativeGetFatal();if(fatal!=null){showFailure("Código "+NativeCoreActivity.nativeGetFatalKind()+"\n"+fatal);return;}
            if(NativeCoreActivity.nativeGetRelaunch()){restartCore();return;}
            if(NativeCoreActivity.nativeGetQuit()){closeGame();return;}
            if(NativeCoreActivity.nativeGetFrameShown()&&loading.getVisibility()!=View.GONE)loading.setVisibility(View.GONE);
            if(showFps&&menu==null){float[] s=NativeCoreActivity.nativeGetFrameStats();if(s!=null&&s.length>=2&&s[0]>0){fps.setText(String.format(java.util.Locale.ROOT,"%.0f FPS · %.1f ms",s[0],s[1]));fps.setVisibility(View.VISIBLE);}}
        }catch(LinkageError e){showFailure("hakuX JNI: "+e.getMessage());return;}
        handler.postDelayed(poll,350);
    }
    private void showFailure(String error){
        if(moving)return;moving=true;syncPause();
        boolean missingTimeline = error.contains("timelineSemaphore");
        boolean missingVersion = error.contains("lacks vulkan1.1");
        String report=error+"\n"+tr("Driver requested: ","Driver solicitado: ","Controlador solicitado: ")
                +(attemptedLibrary.isEmpty()?tr("System","Sistema","Sistema"):attemptedLibrary)
                +(driverError.isEmpty()?"":"\n"+driverError);
        if(missingTimeline)report+="\n"+tr("This Vulkan driver needs timeline semaphore support to run hakuX.",
                "O hakuX exige que o driver Vulkan ofereça semáforos timeline.",
                "hakuX requiere que el controlador Vulkan admita semáforos timeline.");
        android.util.Log.e("HAKUX36",report);saveFailure(report);
        String imported=getSharedPreferences("launcher",MODE_PRIVATE).getString("driver_library","");
        boolean retry=(missingTimeline||missingVersion)&&attemptedLibrary.isEmpty()
                &&!getIntent().getBooleanExtra("hakux38_retry_imported",false)
                &&imported!=null&&!imported.isEmpty()&&imported.equals(new File(imported).getName())
                &&new File(new File(getFilesDir(),"gpu_driver/current"),imported).isFile();
        AlertDialog.Builder dialog=PortUi.dialog(this).setTitle("hakuX · Def Jam")
                .setMessage(report).setCancelable(false)
                .setPositiveButton(tr("Back","Voltar","Volver"),(w,n)->finish())
                .setNeutralButton(tr("Copy error","Copiar erro","Copiar error"),null);
        if(retry)dialog.setNegativeButton(tr("Try imported driver","Tentar driver importado","Probar controlador importado"),(w,n)->{
            getIntent().putExtra("hakux36_driver_library",imported);
            getIntent().putExtra("hakux38_retry_imported",true);
            restartCore(false);
        });
        final String diagnostic=report;
        AlertDialog d=dialog.create();
        d.setOnShowListener(w->{ButtonCopy.install(d,this,diagnostic);if(d.getWindow()!=null)d.getWindow().setBackgroundDrawable(PortUi.shape(this,PortUi.CARD,PortUi.LINE,16));});d.show();
    }
    private static final class ButtonCopy {
        static void install(AlertDialog d,Context c,String report){android.widget.Button b=d.getButton(AlertDialog.BUTTON_NEUTRAL);if(b!=null)b.setOnClickListener(v->{
            ClipboardManager clipboard=(ClipboardManager)c.getSystemService(CLIPBOARD_SERVICE);if(clipboard!=null)clipboard.setPrimaryClip(ClipData.newPlainText("hakuX",report));});}
    }
    private void saveFailure(String report){
        try{File dir=new File(Environment.getExternalStorageDirectory(),"DefJamFFNY/logs");
            if(!dir.isDirectory()&&!dir.mkdirs()){File ext=getExternalFilesDir(null);dir=new File(ext==null?getFilesDir():ext,"logs");if(!dir.isDirectory())dir.mkdirs();}
            try(FileOutputStream f=new FileOutputStream(new File(dir,"hakux36-error-"+System.currentTimeMillis()+".txt"))){f.write(report.getBytes(StandardCharsets.UTF_8));}
        }catch(Exception e){android.util.Log.w("HAKUX36","Could not save report",e);}
    }
    private void openMenu(){
        if(menu!=null||moving)return;controls.reset();String[] items={
            controls.controlsVisible()?tr("Hide touch buttons","Ocultar botões","Ocultar botones"):tr("Show touch buttons","Mostrar botões","Mostrar botones"),
            controls.isStick()?tr("Digital D-pad","Direcional digital","Direccional digital"):tr("Analog stick","Analógico virtual","Analógico virtual"),
            controls.isRightStickVisible()?tr("Hide right stick","Ocultar analógico direito","Ocultar analógico derecho"):tr("Show right stick","Mostrar analógico direito","Mostrar analógico derecho"),
            tr("Change opacity","Alternar opacidade","Cambiar opacidad"),
            showFps?tr("Hide FPS","Ocultar FPS","Ocultar FPS"):tr("Show FPS","Mostrar FPS","Mostrar FPS"),
            tr("Restart without shader preloading","Reiniciar sem pré-carregar shaders","Reiniciar sin precargar shaders"),
            tr("Exit to launcher","Sair para o launcher","Salir al launcher")};
        menu=PortUi.dialog(this).setTitle("hakuX · Def Jam").setItems(items,(d,which)->menuAction(which))
            .setNegativeButton(tr("Resume","Voltar ao jogo","Volver al juego"),null).create();
        menu.setOnDismissListener(d->{menu=null;syncPause();hideBars();});menu.show();syncPause();
    }
    private void menuAction(int which){
        switch(which){case 0:controls.setShown(!controls.controlsVisible());break;
            case 1:controls.setStick(!controls.isStick());runtime.edit().putBoolean("analog",controls.isStick()).apply();break;
            case 2:controls.setRightStick(!controls.isRightStickVisible());runtime.edit().putBoolean("right_stick",controls.isRightStickVisible()).apply();break;
            case 3:int opacity=1-runtime.getInt("opacity",0);controls.setOpacity(opacity);runtime.edit().putInt("opacity",opacity).apply();break;
            case 4:showFps=!showFps;if(!showFps)fps.setVisibility(View.GONE);break;
            case 5:
                saveFailure("Manual restart without shader preloading; driver requested="
                        +(attemptedLibrary.isEmpty()?"system":attemptedLibrary));
                getIntent().putExtra("hakux36_async",false).putExtra("hakux36_prebuild",false);
                restartCore(false);break;
            case 6:closeGame();break;default:break;}
    }
    private void closeGame(){moving=true;syncPause();finish();}
    private void restartCore(){restartCore(true);}
    private void restartCore(boolean guestRelaunch){
        moving=true;syncPause();Intent next=new Intent(this,HakuXRestart36.class).putExtras(getIntent())
            .setData(getIntent().getData()).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            .putExtra(RELAUNCH,guestRelaunch).putExtra(HakuXRestart36.OLD_PID,android.os.Process.myPid());
        if(getIntent().getClipData()!=null)next.setClipData(getIntent().getClipData());
        startActivity(next);finish();android.os.Process.killProcess(android.os.Process.myPid());
    }
}
