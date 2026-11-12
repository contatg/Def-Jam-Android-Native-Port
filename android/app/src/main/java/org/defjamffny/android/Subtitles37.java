package org.defjamffny.android;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.io.IOException;
import java.util.List;
import java.util.function.BooleanSupplier;

/** Story subtitles and language-v1 packs for the native port. */
final class Subtitles37 {
    static final int REQUEST=0x3737;
    private static String tr(String en,String pt,String es){return UiText.isPortuguese()?pt:UiText.isSpanish()?es:en;}
    private static void toast(Activity a,String s){Toast.makeText(a,s,Toast.LENGTH_LONG).show();}
    private static void add(LinearLayout root,View child){
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,-2);p.topMargin=PortUi.dp(root.getContext(),8);root.addView(child,p);
    }
    private static boolean ready(Activity a,BooleanSupplier canWrite){
        if(canWrite.getAsBoolean())return true;
        toast(a,tr("Close the native game before changing language.","Feche o jogo nativo antes de mudar o idioma.","Cierra el juego nativo antes de cambiar el idioma."));return false;
    }
    private static boolean save(Activity a,File root,BooleanSupplier canWrite,String key,String value){
        if(!ready(a,canWrite))return false;
        try{LanguageSettings37.save(root,key,value);return true;}
        catch(IOException e){toast(a,tr("Could not save: ","Não foi possível salvar: ","No se pudo guardar: ")+e.getMessage());return false;}
    }
    private static String value(File root,String key,String fallback){try{return LanguageSettings37.value(root,key,fallback);}catch(IOException e){return fallback;}}
    static void show(Activity a,File root,BooleanSupplier canWrite){
        if(!ready(a,canWrite))return;
        ScrollView scroll=new ScrollView(a);LinearLayout pane=new LinearLayout(a);pane.setOrientation(LinearLayout.VERTICAL);
        pane.setPadding(PortUi.dp(a,18),PortUi.dp(a,8),PortUi.dp(a,18),PortUi.dp(a,16));
        scroll.addView(pane);
        TextView intro=PortUi.text(a,tr("STORY  /  LANGUAGE PACKS","HISTÓRIA  /  PACOTES DE IDIOMA","HISTORIA  /  PAQUETES DE IDIOMA"),11,PortUi.GOLD,true);intro.setLetterSpacing(.06f);add(pane,intro);
        add(pane,PortUi.text(a,tr("Enable subtitles in Story scenes and choose a translator's language pack. Voices are unchanged.","Mostra as legendas que o jogo tem nas cenas da História. Para traduzir o texto, selecione um pacote de idioma.","Activa subtítulos en las escenas de Historia y elige un paquete de un traductor. Las voces no cambian."),12,PortUi.MUTED,false));
        String[] subValues={"auto","all cutscenes","as the game does"};
        String[] subLabels={tr("Automatic (follow the mod)","Automático (segue o mod)","Automático (según el mod)"),tr("All Story scenes","Todas as cenas da História","Todas las escenas de Historia"),tr("Original game behavior","Como no jogo original","Como en el juego original")};
        Button subtitles=PortUi.button(a,"",false);
        Runnable refreshSub=()->{String v=value(root,"story_subtitles","all cutscenes");int i=0;for(int n=0;n<subValues.length;n++)if(subValues[n].equalsIgnoreCase(v))i=n;
            subtitles.setText(tr("Subtitles: ","Legendas: ","Subtítulos: ")+subLabels[i]+"  ▾");};refreshSub.run();
        subtitles.setOnClickListener(v->{String current=value(root,"story_subtitles","all cutscenes");int chosen=0;for(int n=0;n<subValues.length;n++)if(subValues[n].equalsIgnoreCase(current))chosen=n;
            PortUi.show(PortUi.dialog(a).setTitle(tr("Story subtitles","Legendas da História","Subtítulos de Historia"))
                .setSingleChoiceItems(subLabels,chosen,(dialog,which)->{if(save(a,root,canWrite,"story_subtitles",subValues[which]))refreshSub.run();dialog.dismiss();}));});
        add(pane,subtitles);
        Button pack=PortUi.button(a,"",false);
        Runnable refreshPack=()->{String selected=value(root,"pack","");String name=tr("Original game","Jogo original","Juego original");
            for(LanguageMods37.Pack item:LanguageMods37.list(root))if(item.folder.equals(selected))name=item.name;
            if(!selected.isEmpty()&&name.equals(tr("Original game","Jogo original","Juego original")))name=selected+tr(" (missing)"," (ausente)"," (ausente)");
            pack.setText(tr("Language: ","Idioma: ","Idioma: ")+name+"  ▾");};refreshPack.run();
        pack.setOnClickListener(v->{List<LanguageMods37.Pack> packs=LanguageMods37.list(root);String[] labels=new String[packs.size()+1];labels[0]=tr("Original game","Jogo original","Juego original");
            String selected=value(root,"pack","");int chosen=0;for(int i=0;i<packs.size();i++){labels[i+1]=packs.get(i).name;if(packs.get(i).folder.equals(selected))chosen=i+1;}
            PortUi.show(PortUi.dialog(a).setTitle(tr("Select a language","Escolher idioma","Elegir idioma"))
                .setSingleChoiceItems(labels,chosen,(dialog,which)->{if(save(a,root,canWrite,"pack",which==0?"":packs.get(which-1).folder))refreshPack.run();dialog.dismiss();}));});
        add(pane,pack);
        Button choose=PortUi.button(a,tr("Import language pack (.zip)","Importar mod de idioma (.zip)","Importar mod de idioma (.zip)"),true);
        choose.setOnClickListener(v->{if(!ready(a,canWrite))return;
            Intent pick=new Intent(Intent.ACTION_OPEN_DOCUMENT);pick.addCategory(Intent.CATEGORY_OPENABLE);pick.setType("application/zip");
            pick.putExtra(Intent.EXTRA_MIME_TYPES,new String[]{"application/zip","application/x-zip-compressed","application/octet-stream"});
            try{a.startActivityForResult(pick,REQUEST);}catch(ActivityNotFoundException e){toast(a,tr("No file picker found.","Nenhum seletor de arquivos encontrado.","No se encontró un selector de archivos."));}});
        add(pane,choose);
        add(pane,PortUi.text(a,tr("Accepts language-v1 packs with language.ini and .djd files. The game checks the pack against your USA dump; a mismatch falls back to the original language. Importing a console patch ZIP directly is not supported.","Aceita pacotes language-v1 com language.ini e arquivos .djd. O jogo confere o pacote com seu dump USA; se não combinar, usa o idioma original. ZIP de patch de console não é importado diretamente.","Acepta paquetes language-v1 con language.ini y archivos .djd. El juego verifica el paquete con tu copia USA; si no coincide, usa el idioma original. No importa ZIP de parches de consola directamente."),11,PortUi.MUTED,false));
        add(pane,PortUi.text(a,tr("Native mode only. Changes apply on the next launch. No translation is included.","Somente no modo nativo. As mudanças valem ao iniciar o jogo. Nenhuma tradução está incluída.","Solo modo nativo. Los cambios se aplican al iniciar. No incluye traducciones."),11,PortUi.GOLD,false));
        PortUi.show(PortUi.dialog(a).setTitle(tr("Subtitles and mods","Legendas e mods","Subtítulos y mods")).setView(scroll).setPositiveButton(tr("Done","Concluído","Listo"),(d,w)->{}));
    }
    static void picked(Activity a,Intent data,File root,BooleanSupplier canWrite){
        if(data==null||data.getData()==null||!ready(a,canWrite))return;Uri uri=data.getData();
        toast(a,tr("Importing language pack…","Importando pacote de idioma…","Importando paquete de idioma…"));
        new Thread(()->{
            try{
                LanguageMods37.Pack pack=LanguageMods37.importZip(a,uri,root);
                a.runOnUiThread(()->{
                    if(save(a,root,canWrite,"pack",pack.folder)){
                        toast(a,tr("Imported: ","Importado: ","Importado: ")+pack.name+tr(". It will be checked when the game starts.",". Será verificado ao iniciar o jogo.",". Se verificará al iniciar el juego."));
                        show(a,root,canWrite);
                    }else toast(a,tr("Pack imported but not selected.","Pacote importado, mas não selecionado.","Paquete importado, pero no seleccionado."));
                });
            }catch(Exception e){a.runOnUiThread(()->toast(a,tr("Import failed: ","Falha na importação: ","Error al importar: ")+e.getMessage()));}
        },"language-import37").start();
    }
    private Subtitles37(){}
}
