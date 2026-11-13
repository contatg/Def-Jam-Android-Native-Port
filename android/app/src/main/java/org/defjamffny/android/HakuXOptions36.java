// SPDX-License-Identifier: GPL-3.0-or-later
package org.defjamffny.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Toast;
import java.io.File;

/** Settings belong to the embedded emulator and never change the native port. */
final class HakuXOptions36 {
    private static final String PREFIX="hakux36_";
    private static SharedPreferences prefs(Context c){return c.getSharedPreferences("launcher",Context.MODE_PRIVATE);}
    private static String tr(String en,String pt,String es){return UiText.isPortuguese()?pt:UiText.isSpanish()?es:en;}
    private static int dp(Context c,int n){return PortUi.dp(c,n);}
    private static boolean customDriverAvailable(Context c){
        String library=prefs(c).getString("driver_library","");
        return library!=null&&!library.isEmpty()&&library.equals(new File(library).getName())
            &&new File(new File(c.getFilesDir(),"gpu_driver/current"),library).isFile();
    }
    static void putExtras(Context c,Intent out){
        SharedPreferences p=prefs(c);
        out.putExtra(PREFIX+"scale",p.getInt(PREFIX+"scale",1));
        out.putExtra(PREFIX+"aspect",p.getString(PREFIX+"aspect","auto"));
        out.putExtra(PREFIX+"upscale",p.getString(PREFIX+"upscale","bilinear"));
        for(String key:new String[]{"fxaa","uncap","a610_compat"})out.putExtra(PREFIX+key,p.getBoolean(PREFIX+key,false));
        for(String key:new String[]{"async","prebuild","touch","fps"})out.putExtra(PREFIX+key,p.getBoolean(PREFIX+key,true));
        out.putExtra(PREFIX+"fps_style",p.getString(PREFIX+"fps_style","compact"));
        if(p.getBoolean(PREFIX+"custom_driver",false)){
            if(customDriverAvailable(c))out.putExtra(PREFIX+"driver_library",p.getString("driver_library",""));
            else Toast.makeText(c,tr("Imported driver missing; using the system driver.","Driver importado ausente; usando o driver do sistema.","Falta el controlador importado; se usará el del sistema."),Toast.LENGTH_LONG).show();
        }
    }
    static HakuXConfig36 config(Intent in){
        return new HakuXConfig36(in.getIntExtra(PREFIX+"scale",1),in.getStringExtra(PREFIX+"aspect"),
            in.getStringExtra(PREFIX+"upscale"),in.getBooleanExtra(PREFIX+"fxaa",false),
            in.getBooleanExtra(PREFIX+"async",true),in.getBooleanExtra(PREFIX+"prebuild",true),
            in.getBooleanExtra(PREFIX+"uncap",false),in.getBooleanExtra(PREFIX+"touch",true),in.getBooleanExtra(PREFIX+"fps",true));
    }
    private static void add(LinearLayout root, View view){
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,-2);p.topMargin=dp(root.getContext(),7);root.addView(view,p);
    }
    private static Button choice(Activity a,LinearLayout root,String title,String[] labels,String[] values,String key,String defaultValue){
        SharedPreferences prefs=prefs(a);Button b=PortUi.button(a,"",false);
        Runnable update=()->{String selected=prefs.getString(PREFIX+key,defaultValue);int index=0;
            for(int i=0;i<values.length;i++)if(values[i].equals(selected)){index=i;break;}b.setText(title+":  "+labels[index]+"  ▾");};
        update.run();b.setOnClickListener(v->{int current=0;String selected=prefs.getString(PREFIX+key,defaultValue);
            for(int i=0;i<values.length;i++)if(values[i].equals(selected)){current=i;break;}
            PortUi.show(PortUi.dialog(a).setTitle(title).setSingleChoiceItems(labels,current,(dialog,which)->{
                prefs.edit().putString(PREFIX+key,values[which]).apply();update.run();dialog.dismiss();}));});
        add(root,b);return b;
    }
    private static Button toggle(Activity a,LinearLayout root,String title,String key,boolean def){
        SharedPreferences prefs=prefs(a);Button b=PortUi.button(a,"",false);
        Runnable update=()->b.setText(title+":  "+(prefs.getBoolean(PREFIX+key,def)?tr("On","Ligado","Activado"):tr("Off","Desligado","Desactivado")));
        update.run();b.setOnClickListener(v->{prefs.edit().putBoolean(PREFIX+key,!prefs.getBoolean(PREFIX+key,def)).apply();update.run();});
        add(root,b);return b;
    }
    static void show(Activity a){
        SharedPreferences p=prefs(a);ScrollView scroll=new ScrollView(a);LinearLayout root=new LinearLayout(a);
        root.setOrientation(LinearLayout.VERTICAL);root.setPadding(dp(a,18),dp(a,8),dp(a,18),dp(a,12));scroll.addView(root);
        add(root,PortUi.text(a,tr("Settings apply when the emulated game starts. The native port keeps its own settings.","As opções entram em vigor ao iniciar a emulação. O port nativo mantém suas próprias configurações.","Las opciones se aplican al iniciar la emulación. El port nativo conserva su configuración."),12,PortUi.MUTED,false));
        Button scale=PortUi.button(a,"",false);Runnable showScale=()->scale.setText(tr("Internal resolution","Resolução interna","Resolución interna")+":  "+p.getInt(PREFIX+"scale",1)+"x  ▾");
        showScale.run();scale.setOnClickListener(v->PortUi.show(PortUi.dialog(a).setTitle(tr("Internal resolution","Resolução interna","Resolución interna"))
            .setSingleChoiceItems(new String[]{"1x", "2x", "3x", "4x"},Math.max(0,Math.min(3,p.getInt(PREFIX+"scale",1)-1)),(d,i)->{p.edit().putInt(PREFIX+"scale",i+1).apply();showScale.run();d.dismiss();})));add(root,scale);
        choice(a,root,tr("Picture shape","Proporção","Proporción"),new String[]{tr("Automatic","Automática","Automática"),"4:3","16:9",tr("Stretch","Esticar","Estirar")},new String[]{"auto","4:3","16:9","stretch"},"aspect","auto");
        choice(a,root,tr("Upscaling","Ampliação","Escalado"),new String[]{"Bilinear","Snapdragon GSR"},new String[]{"bilinear","sgsr"},"upscale","bilinear");
        toggle(a,root,"FXAA","fxaa",false);
        toggle(a,root,tr("Compile shaders asynchronously","Shaders assíncronos","Shaders asíncronos"),"async",true);
        toggle(a,root,tr("Preload cached shaders","Pré-carregar shaders salvos","Precargar shaders guardados"),"prebuild",true);
        toggle(a,root,tr("Touch controls","Controles na tela","Controles táctiles"),"touch",true);
        toggle(a,root,tr("FPS counter","Contador de FPS","Contador FPS"),"fps",true);
        choice(a,root,tr("FPS counter style","Estilo do contador de FPS","Estilo del contador de FPS"),
            new String[]{tr("Compact","Compacto","Compacto"),tr("Classic","Antigo","Clásico")},
            new String[]{"compact","classic"},"fps_style","compact");
        toggle(a,root,tr("Uncapped timing (experimental)","Destravar ritmo (experimental)","Ritmo desbloqueado (experimental)"),"uncap",false);
        add(root,PortUi.text(a,tr("Uncapped timing may alter game speed. Frame generation is available only in native Vulkan mode.","Destravar o ritmo pode alterar a velocidade do jogo. O framegen permanece apenas no Vulkan nativo.","Desbloquear el ritmo puede cambiar la velocidad. El framegen está solo en Vulkan nativo."),11,PortUi.MUTED,false));
        Button driver=PortUi.button(a,"",false);Runnable showDriver=()->driver.setText(tr("Driver","Driver","Controlador")+":  "+
            (p.getBoolean(PREFIX+"custom_driver",false)?tr("Imported Turnip","Turnip importado","Turnip importado"):tr("System","Sistema","Sistema"))+"  ▾");
        showDriver.run();driver.setOnClickListener(v->PortUi.show(PortUi.dialog(a).setTitle("Vulkan driver")
            .setItems(new String[]{tr("System","Sistema","Sistema"),tr("Use imported Turnip","Usar Turnip importado","Usar Turnip importado")},(d,n)->{
                if(n==1&&!customDriverAvailable(a)){Toast.makeText(a,tr("Import a Turnip driver in the launcher first.","Importe um driver Turnip no launcher primeiro.","Importa primero un controlador Turnip en el launcher."),Toast.LENGTH_LONG).show();return;}
                p.edit().putBoolean(PREFIX+"custom_driver",n==1).apply();showDriver.run();})));add(root,driver);
        add(root,PortUi.text(a,tr("Imported drivers are experimental in the emulator; the system driver is the default.","O driver importado é experimental no emulador; o padrão é o driver do sistema.","El controlador importado es experimental en el emulador; el sistema es el predeterminado."),11,PortUi.MUTED,false));
        toggle(a,root,tr("A610 Turnip compatibility (experimental)","Compatibilidade A610 Turnip (experimental)","Compatibilidad A610 Turnip (experimental)"),"a610_compat",false);
        add(root,PortUi.text(a,tr("Only affects an imported Turnip in the emulator; it may still be incompatible.","Afeta apenas o Turnip importado no emulador; ainda pode haver incompatibilidade.","Afecta solo a Turnip importado en el emulador; aún puede ser incompatible."),11,PortUi.MUTED,false));
        Button reset=PortUi.button(a,tr("Restore emulator defaults","Restaurar padrão do emulador","Restaurar valores del emulador"),false);add(root,reset);
        AlertDialog dialog=PortUi.dialog(a).setTitle(tr("Emulator options","Opções do emulador","Opciones del emulador"))
            .setView(scroll).setPositiveButton(tr("Done","Pronto","Listo"),null).create();
        reset.setOnClickListener(v->PortUi.show(PortUi.dialog(a).setTitle(tr("Restore emulator defaults?","Restaurar opções do emulador?","¿Restaurar opciones del emulador?"))
            .setNegativeButton(tr("Cancel","Cancelar","Cancelar"),null).setPositiveButton(tr("Restore","Restaurar","Restaurar"),(d,w)->{
                SharedPreferences.Editor e=p.edit();for(String key:p.getAll().keySet())if(key.startsWith(PREFIX))e.remove(key);e.apply();dialog.dismiss();show(a);}))); 
        dialog.setOnShowListener(v->{for(int id:new int[]{AlertDialog.BUTTON_POSITIVE,AlertDialog.BUTTON_NEGATIVE}){Button b=dialog.getButton(id);if(b!=null)b.setTextColor(PortUi.GOLD);}if(dialog.getWindow()!=null)dialog.getWindow().setBackgroundDrawable(PortUi.shape(a,PortUi.CARD,PortUi.LINE,16));});
        dialog.show();
    }
    private HakuXOptions36(){}
}
