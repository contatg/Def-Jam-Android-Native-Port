package org.defjamffny.android;

import android.content.Context;
import android.net.Uri;
import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.UUID;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Imports the upstream language-v1 ZIP into its own folder, never into game/. */
final class LanguageMods37 {
    private static final long MAX_TOTAL=512L*1024*1024, MAX_FILE=128L*1024*1024;
    private static final int MAX_ENTRIES=4096;
    static final class Pack { final String folder,name; Pack(String folder,String name){this.folder=folder;this.name=name;} }
    private static boolean folderName(String s){return s!=null&&s.matches("[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}");}
    static List<Pack> list(File root) {
        List<Pack> packs=new ArrayList<>();File[] dirs=new File(root,"mods").listFiles();
        if(dirs==null)return packs;Arrays.sort(dirs,(a,b)->a.getName().compareToIgnoreCase(b.getName()));
        for(File dir:dirs)if(dir.isDirectory()&&folderName(dir.getName())){
            File ini=new File(dir,"language.ini");if(ini.isFile()&&ini.length()>0&&ini.length()<=65536){
                String name=dir.getName();try{name=displayName(ini,name);}catch(IOException ignored){}
                packs.add(new Pack(dir.getName(),name));
            }
        }
        return packs;
    }
    private static String displayName(File ini,String fallback) throws IOException {
        if(ini.length()>65536)throw new IOException("language.ini is too large");
        byte[] b=new byte[(int)ini.length()];try(InputStream in=new FileInputStream(ini)){int at=0,n;while(at<b.length&&(n=in.read(b,at,b.length-at))>0)at+=n;if(at!=b.length)throw new IOException("Could not read language.ini");}
        String section="";boolean language=false,schema=false,files=false;String name=fallback;
        for(String line:new String(b,StandardCharsets.UTF_8).replace("\ufeff","").split("\\r?\\n")){
            line=line.trim();if(line.startsWith(";")||line.startsWith("#")||line.isEmpty())continue;
            if(line.startsWith("[")&&line.endsWith("]")){section=line.substring(1,line.length()-1).trim().toLowerCase(Locale.ROOT);if(section.equals("language"))language=true;if(section.equals("files"))files=true;continue;}
            int eq=line.indexOf('=');if(eq<1||!section.equals("language"))continue;
            String k=line.substring(0,eq).trim(),v=line.substring(eq+1).trim();
            if(k.equalsIgnoreCase("schema")&&v.equals("1"))schema=true;
            if(k.equalsIgnoreCase("name")&&!v.isEmpty())name=v.length()>64?v.substring(0,64):v;
        }
        if(!language||!schema||!files)throw new IOException("ZIP precisa conter um pacote de idioma language-v1 valido");
        return name;
    }
    private static void remove(File path){File[] children=path.listFiles();if(children!=null)for(File f:children)remove(f);path.delete();}
    private static String safePath(String raw) throws IOException {
        if(raw==null||raw.isEmpty()||raw.startsWith("/")||raw.indexOf('\\')>=0||raw.indexOf(':')>=0||raw.length()>240)throw new IOException("Caminho inseguro no ZIP");
        String[] pieces=raw.split("/",-1);
        for(int i=0;i<pieces.length;i++){
            String p=pieces[i];if(p.isEmpty()&&i==pieces.length-1)continue;
            if(p.isEmpty()||p.equals(".")||p.equals("..")||p.charAt(0)=='.')throw new IOException("Caminho inseguro no ZIP");
        }
        return raw;
    }
    static Pack importZip(Context context,Uri uri,File root) throws IOException {
        try(InputStream opened=context.getContentResolver().openInputStream(uri)){
            if(opened==null)throw new IOException("Cannot open ZIP");
            return importStream(opened,root);
        }
    }
    static Pack importStream(InputStream opened,File root) throws IOException {
        File mods=new File(root,"mods");if(!mods.isDirectory()&&!mods.mkdirs())throw new IOException("Cannot create mods folder");
        File stage=new File(mods,".import37-"+UUID.randomUUID());if(!stage.mkdir())throw new IOException("Cannot create temporary folder");
        try {
            long total=0;int entries=0;byte[] buffer=new byte[32768];
            try(ZipInputStream zip=new ZipInputStream(new BufferedInputStream(opened))){
                ZipEntry entry;
                while((entry=zip.getNextEntry())!=null){
                    if(++entries>MAX_ENTRIES)throw new IOException("ZIP possui arquivos demais");
                    String rel=safePath(entry.getName());File out=new File(stage,rel);
                    if(entry.isDirectory()){if(!out.isDirectory()&&!out.mkdirs())throw new IOException("Cannot create folder");continue;}
                    String lower=out.getName().toLowerCase(Locale.ROOT);
                    if(!(lower.endsWith(".ini")||lower.endsWith(".djd")||lower.endsWith(".png")||lower.endsWith(".dds")||lower.endsWith(".md")||lower.endsWith(".txt")||lower.endsWith(".json")))throw new IOException("Formato nao aceito: "+out.getName());
                    File parent=out.getParentFile();if(!parent.isDirectory()&&!parent.mkdirs())throw new IOException("Cannot create folder");
                    if(out.exists())throw new IOException("Arquivo repetido no ZIP");
                    long size=0;try(FileOutputStream file=new FileOutputStream(out)){
                        int n;while((n=zip.read(buffer))!=-1){size+=n;total+=n;if(size>MAX_FILE||total>MAX_TOTAL)throw new IOException("ZIP excede o limite de tamanho");file.write(buffer,0,n);}
                    }
                }
            }
            File content=stage;File ini=new File(content,"language.ini");String suggested="";
            if(!ini.isFile()){
                File[] children=stage.listFiles();if(children==null||children.length!=1||!children[0].isDirectory())throw new IOException("ZIP deve conter uma pasta de idioma com language.ini");
                content=children[0];suggested=content.getName();ini=new File(content,"language.ini");
            }
            if(!ini.isFile())throw new IOException("language.ini ausente");
            if(!folderName(suggested))suggested="idioma-"+System.currentTimeMillis();
            String label=displayName(ini,suggested);String candidate=suggested;
            for(int i=1;new File(mods,candidate).exists();i++){
                if(i>9999)throw new IOException("Cannot choose a unique folder");
                String prefix=suggested.length()>58?suggested.substring(0,58):suggested;candidate=prefix+"-"+i;
            }
            File destination=new File(mods,candidate);if(!content.renameTo(destination))throw new IOException("Cannot install language folder");
            return new Pack(candidate,label);
        }finally{if(stage.exists())remove(stage);}
    }
    private LanguageMods37(){}
}
