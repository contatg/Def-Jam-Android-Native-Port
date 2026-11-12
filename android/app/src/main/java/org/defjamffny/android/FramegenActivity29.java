package org.defjamffny.android;

import android.app.*;
import android.content.*;
import android.graphics.Color;
import android.net.Uri;
import android.os.*;
import android.widget.*;
import java.io.*;
import java.util.*;

/** Runs in the launcher process, also when opened from the in-game gear. */
public final class FramegenActivity29 extends Activity {
    private static final int PICK=2907;
    private final File root=new File(Environment.getExternalStorageDirectory(),"DefJamFFNY");
    private FramegenControls31.Choice engine,multiplier,refresh;private SeekBar flow;private TextView status,flowLabel;
    private Button importer,save;private boolean importing,dirty;
    private SharedPreferences prefs(){return getSharedPreferences("launcher",MODE_PRIVATE);}
    private int dp(float n){return Math.round(n*getResources().getDisplayMetrics().density);}
    private String tr(String en,String pt,String es){return UiText.isPortuguese()?pt:UiText.isSpanish()?es:en;}
    private void error(Exception e){if(isFinishing()||isDestroyed())return;new AlertDialog.Builder(this,android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle(tr("Could not apply","Não foi possível aplicar","No se pudo aplicar")).setMessage(e.getMessage()).setPositiveButton("OK",null).show();}
    private TextView text(String s){TextView t=new TextView(this);t.setText(s);t.setTextColor(Color.WHITE);t.setTypeface(null,android.graphics.Typeface.NORMAL);t.setPadding(dp(8),dp(10),dp(8),dp(10));return t;}
    @Override public void onCreate(Bundle state){super.onCreate(state);UiText.setLanguage(prefs().getString("ui_language","en"));render();}
    private android.graphics.drawable.GradientDrawable shape(int color,int border){android.graphics.drawable.GradientDrawable d=new android.graphics.drawable.GradientDrawable();d.setColor(color);d.setCornerRadius(dp(16));d.setStroke(1,border);return d;}
    private LinearLayout card(LinearLayout parent,String title){LinearLayout c=new LinearLayout(this);c.setOrientation(LinearLayout.VERTICAL);c.setPadding(dp(18),dp(10),dp(18),dp(14));c.setBackground(shape(0xff20232c,0xff4b4231));LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,-2);lp.setMargins(0,dp(10),0,dp(6));parent.addView(c,lp);TextView heading=text(title);heading.setTextColor(0xffe3bb73);heading.setTextSize(17);c.addView(heading);return c;}
    private Button button(LinearLayout parent,String title,boolean primary){Button b=new Button(this);b.setText(title);b.setTextColor(primary?0xff17191f:0xffe3bb73);b.setAllCaps(false);b.setBackground(shape(primary?0xffe3bb73:0xff292d37,0xff5a4c33));LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,dp(50));lp.setMargins(dp(4),dp(8),dp(4),dp(4));parent.addView(b,lp);return b;}
    private FramegenControls31.Choice choices(LinearLayout parent,String label,String...values){
        parent.addView(text(label));FramegenControls31.Choice choice=new FramegenControls31.Choice(this,label,values);
        LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,-2);lp.setMargins(dp(4),dp(2),dp(4),dp(10));parent.addView(choice,lp);return choice;
    }
    private void changed(){dirty=true;updateStatus();}
    private void render(){
        LinearLayout box=new LinearLayout(this);box.setOrientation(LinearLayout.VERTICAL);box.setPadding(dp(20),dp(12),dp(20),dp(18));box.setBackgroundColor(0xff111319);
        ScrollView scroll=new ScrollView(this);scroll.setFillViewport(true);scroll.setFitsSystemWindows(true);scroll.addView(box);setContentView(scroll);
        TextView brand=text("FIGHT FOR NY");brand.setTextSize(26);brand.setTypeface(null,1);brand.setTextColor(0xffe3bb73);box.addView(brand);
        box.addView(text(tr("Frame generation","Geração de quadros","Generación de cuadros")));
        LsfgSettings27.State current=new LsfgSettings27.State(0,25);
        try{current=LsfgSettings27.read(root);}catch(Exception e){error(e);}
        if(prefs().getBoolean("lsfg27_pending",false))current=new LsfgSettings27.State(prefs().getInt("fg_engine39_pending",0),prefs().getInt("lsfg27_flow",25),prefs().getInt("lsfg30_multiplier",2),prefs().getInt("lsfg30_refresh",60));
        LinearLayout main=card(box,tr("Engine","Motor","Motor"));
        engine=choices(main,tr("Selected engine","Motor selecionado","Motor seleccionado"),
            tr("Off","Desligado","Desactivado"),"Win-FG v37","Lossless / LSFG");
        engine.setSelection(current.engine);
        main.addView(text(tr("LSFG runs in menus, cutscenes and fights. Win-FG is limited to fights.","O LSFG roda em menus, cenas e lutas. O Win-FG fica só nas lutas.","LSFG funciona en menús, escenas y peleas. Win-FG se limita a las peleas.")));
        LinearLayout quality=card(box,tr("Fluidity & quality","Fluidez e qualidade","Fluidez y calidad"));
        multiplier=choices(quality,tr("Multiplier","Multiplicador","Multiplicador"),"x2","x3","x4");multiplier.setSelection(current.multiplier-2);
        refresh=choices(quality,tr("Output ceiling","Limite de saída","Límite de salida"),"60 Hz","90 Hz","120 Hz");refresh.setSelection(current.refresh==120?2:current.refresh==90?1:0);
        quality.addView(text(tr("The effective multiplier is limited by the display and GPU. x3/x4 cost more GPU time; the real game speed stays unchanged.","O multiplicador efetivo é limitado pela tela e GPU. x3/x4 exigem mais GPU; a velocidade real do jogo continua igual.","La pantalla y GPU limitan el multiplicador efectivo. x3/x4 cuestan más GPU; la velocidad real no cambia.")));
        quality.addView(text(tr("Recommended for x2: set the in-game FPS limit to 30 for 60 Hz, or 60 for 120 Hz.","Recomendado para x2: no jogo, limite em 30 FPS para 60 Hz ou 60 FPS para 120 Hz.","Recomendado para x2: en el juego, limita a 30 FPS para 60 Hz o a 60 FPS para 120 Hz.")));
        flowLabel=text("");quality.addView(flowLabel);flow=new SeekBar(this);FramegenControls31.slider(flow);flow.setMax(75);flow.setProgress(current.flow-25);quality.addView(flow);
        flow.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){public void onStartTrackingTouch(SeekBar b){}public void onStopTrackingTouch(SeekBar b){}public void onProgressChanged(SeekBar b,int v,boolean user){flowLabel.setText("Flow Scale: "+(v+25)+"%");if(user)changed();}});flowLabel.setText("Flow Scale: "+(flow.getProgress()+25)+"%");
        LinearLayout files=card(box,tr("Lossless library","Biblioteca Lossless","Biblioteca Lossless"));status=text("");status.setTextColor(0xffd4c5a7);files.addView(status);updateStatus();
        importer=button(files,tr("Import Lossless.dll","Importar Lossless.dll","Importar Lossless.dll"),false);
        importer.setOnClickListener(v->{if(importing)return;try{if(gamePid()!=0)throw new IOException(tr("Close the game before replacing shaders.","Feche o jogo antes de substituir shaders.","Cierra el juego antes de reemplazar shaders."));Intent pick=new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE);startActivityForResult(pick,PICK);}catch(Exception e){error(e);}});
        save=button(box,tr("Apply settings","Aplicar configurações","Aplicar configuración"),true);save.setOnClickListener(v->apply());
        Button defaults=button(box,tr("Restore safe defaults","Restaurar padrão seguro","Restaurar valores seguros"),false);defaults.setOnClickListener(v->{engine.setSelection(0);flow.setProgress(0);multiplier.setSelection(0);refresh.setSelection(0);changed();});
        Button back=button(box,tr("Back","Voltar","Atrás"),false);back.setOnClickListener(v->onBackPressed());
        multiplier.onChanged(this::changed);refresh.onChanged(this::changed);
        engine.onChanged(this::changed);dirty=false;updateStatus();
    }
    private String engineLabel(int mode){return mode==1?"Win-FG v37":mode==2?"Lossless / LSFG":tr("Off","Desligado","Desactivado");}
    private void updateStatus(){
        if(status==null)return;
        String saved;
        try{LsfgSettings27.State state=LsfgSettings27.read(root);saved=tr("Saved for next session: ","Salvo para a próxima sessão: ","Guardado para la próxima sesión: ")+engineLabel(state.engine)+" · x"+state.multiplier+" · "+state.refresh+" Hz";}
        catch(Exception e){saved=tr("Cannot read saved settings: ","Não foi possível ler opções salvas: ","No se pudo leer la configuración: ")+e.getMessage();}
        status.setText(saved+"\n"+(dirty?tr("UNSAVED CHANGES · tap Apply","ALTERAÇÕES NÃO SALVAS · toque Aplicar","CAMBIOS SIN GUARDAR · toca Aplicar")+"\n":"")+(prefs().getBoolean("lsfg27_pending",false)?tr("Waiting for game restart","Aguardando reinício do jogo","Esperando reinicio del juego")+"\n":"")+(LsfgSettings27.shadersReady(root)?tr("Lossless shaders imported","Shaders Lossless importados","Shaders Lossless importados"):tr("Import your Lossless.dll to use LSFG","Importe sua Lossless.dll para usar LSFG","Importa tu Lossless.dll para usar LSFG")));
    }
    private int gamePid(){ActivityManager m=(ActivityManager)getSystemService(ACTIVITY_SERVICE);java.util.List<ActivityManager.RunningAppProcessInfo> ps=m==null?null:m.getRunningAppProcesses();if(ps==null)return -1;for(ActivityManager.RunningAppProcessInfo p:ps)if(p.uid==android.os.Process.myUid()&&(getPackageName()+":game").equals(p.processName))return p.pid;return 0;}
    private void apply(){
        if(importing)return;
        int mode=engine.getSelectedItemPosition();boolean on=mode!=0;
        if(mode==2&&!LsfgSettings27.shadersReady(root)){error(new IOException(tr("Import Lossless.dll first","Importe a Lossless.dll primeiro","Importa Lossless.dll primero")));return;}
        try{if(on&&LsfgSettings27.effectsConflict(root)){new AlertDialog.Builder(this,android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle(tr("Image effects conflict","Conflito de efeitos","Conflicto de efectos")).setMessage(tr("LSFG requires FSR1, spatial upscaling and screen filters off. Disable them and apply?","O LSFG exige FSR1, ampliação espacial e filtros de tela desligados. Desligar esses efeitos e aplicar?","LSFG requiere FSR1, escalado espacial y filtros desactivados. ¿Desactivarlos y aplicar?" )).setPositiveButton(tr("Apply","Aplicar","Aplicar"),(d,w)->stage(true)).setNegativeButton(tr("Cancel","Cancelar","Cancelar"),null).show();}else stage(false);}catch(Exception e){error(e);}
    }
    private void stage(boolean clearEffects){
        try{
            if(!prefs().edit().putBoolean("lsfg27_pending",true).putInt("fg_engine39_pending",engine.getSelectedItemPosition()).putInt("lsfg27_flow",flow.getProgress()+25).putInt("lsfg30_multiplier",multiplier.getSelectedItemPosition()+2).putInt("lsfg30_refresh",refresh.getSelectedItemPosition()==2?120:refresh.getSelectedItemPosition()==1?90:60).putBoolean("lsfg27_clear_effects",clearEffects).putInt("framegen_engine29",engine.getSelectedItemPosition()).commit())throw new IOException("Could not save pending options");
            dirty=false;int pid=gamePid();if(pid<0)throw new IOException(tr("Close the game before applying","Feche o jogo antes de aplicar","Cierra el juego antes de aplicar"));
            if(pid==0){LsfgSettings27.save(root,new LsfgSettings27.State(engine.getSelectedItemPosition(),flow.getProgress()+25,multiplier.getSelectedItemPosition()+2,refresh.getSelectedItemPosition()==2?120:refresh.getSelectedItemPosition()==1?90:60),clearEffects);if(!prefs().edit().remove("lsfg27_pending").remove("lsfg27_clear_effects").commit())throw new IOException("Could not confirm saved settings");updateStatus();Toast.makeText(this,tr("Saved: ","Salvo: ","Guardado: ")+engineLabel(engine.getSelectedItemPosition())+" · x"+(multiplier.getSelectedItemPosition()+2)+" · "+tr("restart the game to apply","reinicie o jogo para aplicar","reinicia para aplicar"),Toast.LENGTH_SHORT).show();return;}
            updateStatus();new AlertDialog.Builder(this,android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle(tr("Restart required","Precisa reiniciar","Reinicio necesario")).setMessage(tr("Frame generation and Flow Scale need a new session. Restarting ends this match. Later applies at the next launch.","Geração de quadros e Flow Scale precisam de uma nova sessão. Reiniciar encerra esta partida. Depois aplica na próxima abertura.","La generación y Flow Scale necesitan una nueva sesión. Reiniciar termina la partida." )).setPositiveButton(tr("Restart now","Reiniciar agora","Reiniciar ahora"),(d,w)->{int game=gamePid();if(game<0){error(new IOException("Cannot identify game process"));return;}if(game>0)android.os.Process.killProcess(game);waitRestart(0);}).setNegativeButton(tr("Later","Depois","Después"),null).show();
        }catch(Exception e){error(e);}
    }
    private void waitRestart(int n){if(isFinishing())return;int pid=gamePid();if(pid==0){startActivity(new Intent(this,LauncherActivity.class).putExtra("restart_game148",true).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK|Intent.FLAG_ACTIVITY_CLEAR_TOP));finish();return;}if(n>=60){error(new IOException("Game process has not stopped; changes remain pending"));return;}new Handler(Looper.getMainLooper()).postDelayed(()->waitRestart(n+1),50);}
    @Override protected void onActivityResult(int request,int result,Intent intent){super.onActivityResult(request,result,intent);if(request!=PICK||result!=RESULT_OK||intent==null||intent.getData()==null)return;
        Uri uri=intent.getData();importing=true;importer.setEnabled(false);save.setEnabled(false);status.setText(tr("Importing shaders…","Importando shaders…","Importando shaders…"));
        new Thread(()->{File staging=new File(root,"lsfg-import29-"+UUID.randomUUID());try{
            if(gamePid()!=0)throw new IOException("Close the game before importing");
            try(InputStream in=getContentResolver().openInputStream(uri)){if(in==null)throw new IOException("Cannot read DLL");LosslessImport29.extract(in,staging);}
            if(gamePid()!=0)throw new IOException("Game started during import; original cache preserved");
            File current=new File(root,"lsfg27"),backup=new File(root,"lsfg27-backup-"+UUID.randomUUID());boolean moved=current.exists();
            if(moved&&!current.renameTo(backup))throw new IOException("Cannot preserve previous shaders");
            if(!staging.renameTo(current)){if(moved&&!backup.renameTo(current))throw new IOException("Import failed; recover shaders from "+backup);throw new IOException("Cannot install shaders");}
            runOnUiThread(()->Toast.makeText(this,tr("50 shaders imported","50 shaders importados","50 shaders importados"),Toast.LENGTH_LONG).show());
        }catch(Exception e){LosslessImport29.delete(staging);runOnUiThread(()->error(e));}finally{runOnUiThread(()->{importing=false;importer.setEnabled(true);save.setEnabled(true);updateStatus();});}},"LosslessImport29").start();
    }
}
