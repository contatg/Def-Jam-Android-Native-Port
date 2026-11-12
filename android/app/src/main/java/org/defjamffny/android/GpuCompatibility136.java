package org.defjamffny.android;

import android.app.Activity;
import android.content.Context;

/** Explicit compatibility fallbacks, not performance presets or vendor detection. */
final class GpuCompatibility136 {
    static int valid(int mode){return mode>=0&&mode<=3?mode:0;}
    static boolean serial(int mode){return (valid(mode)&1)!=0;}
    static boolean conservative(int mode){return (valid(mode)&2)!=0;}
    static int read(Context context){return valid(context.getSharedPreferences("launcher",0).getInt("gpu_compat136",0));}
    private static String tr(String en,String pt,String es){return UiText.isPortuguese()?pt:UiText.isSpanish()?es:en;}
    static void show(Activity a){
        String[] choices={
            tr("Default (recommended)","Padrão (recomendado)","Predeterminado (recomendado)"),
            tr("Conservative synchronization","Sincronização conservadora","Sincronización conservadora"),
            tr("Conservative rendering","Renderização conservadora","Renderizado conservador"),
            tr("Both fallbacks","Ambos os ajustes","Ambos ajustes")};
        PortUi.show(PortUi.dialog(a).setTitle("Fix Mali / PowerVR")
            .setSingleChoiceItems(choices,read(a),(d,index)->{
                a.getSharedPreferences("launcher",0).edit().putInt("gpu_compat136",valid(index)).apply();d.dismiss();
                android.widget.Toast.makeText(a,tr("Applies after restarting the game. Compatibility modes may reduce FPS.","Vale após reiniciar o jogo. Modos de compatibilidade podem reduzir o FPS.","Se aplica al reiniciar el juego. Los modos de compatibilidad pueden reducir los FPS."),android.widget.Toast.LENGTH_LONG).show();
            }).setNegativeButton(UiText.t("Cancel"),null));
    }
    private GpuCompatibility136(){}
}
