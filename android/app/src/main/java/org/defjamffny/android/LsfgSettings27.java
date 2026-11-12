package org.defjamffny.android;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;

/** Launcher settings only. Never loads a driver or changes a running guest. */
final class LsfgSettings27 {
    static final class State {
        final boolean enabled;
        final int engine, flow, multiplier, refresh;
        State(int engine,int flow){this(engine,flow,2,60);}
        State(int engine,int flow,int multiplier,int refresh) {
            this.engine=engine==1||engine==2?engine:0;
            this.enabled=this.engine!=0;this.flow=Math.max(25,Math.min(100,flow));
            this.multiplier=Math.max(2,Math.min(4,multiplier));
            this.refresh=refresh==90||refresh==120?refresh:60;
        }
    }

    private static byte[] readRaw(File file)throws IOException {
        if(!file.exists())return new byte[0];
        if(!file.isFile()||file.length()>1024*1024)throw new IOException("Invalid settings.ini size");
        return Files.readAllBytes(file.toPath());
    }
    private static String decode(byte[] raw)throws IOException {
        return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw)).toString();
    }
    private static List<String> lines(String text) {
        List<String> result=new ArrayList<>();int start=0;
        for(int i=0;i<text.length();i++)if(text.charAt(i)=='\r'||text.charAt(i)=='\n') {
            if(text.charAt(i)=='\r'&&i+1<text.length()&&text.charAt(i+1)=='\n')i++;
            result.add(text.substring(start,i+1));start=i+1;
        }
        if(start<text.length())result.add(text.substring(start));
        return result;
    }
    private static String clean(String text) {
        int a=text.indexOf(';'),b=text.indexOf('#');
        int end=a<0?b:b<0?a:Math.min(a,b);
        return (end<0?text:text.substring(0,end)).trim();
    }
    private static String section(String text) {
        text=clean(text);
        return text.startsWith("[")&&text.endsWith("]")?
            text.substring(1,text.length()-1).trim().toLowerCase(Locale.ROOT):null;
    }
    private static String value(byte[] raw,String key,String fallback)throws IOException {
        String text=decode(raw),current="",result=fallback;
        if(text.startsWith("\ufeff"))text=text.substring(1);
        for(String line:lines(text)) {
            String next=section(line);if(next!=null){current=next;continue;}
            if(!current.equals("graphics"))continue;
            String s=clean(line);int eq=s.indexOf('=');
            if(eq>0&&s.substring(0,eq).trim().equalsIgnoreCase(key))result=s.substring(eq+1).trim();
        }
        return result;
    }
    private static int integer(String s,int fallback) {
        try{return Integer.parseInt(s);}catch(NumberFormatException ignored){return fallback;}
    }
    static State read(File root)throws IOException {
        byte[] raw=readRaw(new File(root,"settings.ini"));
        int engine=integer(value(raw,"fg_engine39","0"),0);
        return new State(engine,integer(value(raw,"lsfg27_flow","25"),25),integer(value(raw,"lsfg30_multiplier","2"),2),integer(value(raw,"lsfg30_refresh","60"),60));
    }
    static boolean effectsConflict(File root)throws IOException {
        byte[] raw=readRaw(new File(root,"settings.ini"));
        return integer(value(raw,"fsr144","0"),0)!=0||integer(value(raw,"screen_filter144","0"),0)!=0||integer(value(raw,"spatial34","0"),0)!=0;
    }
    static boolean shadersReady(File root) {
        int[] ids={255,256,280,281,282,283,284,285,286,287,288,289,290,291,292,293,294,295,296,297,298,299,300,301,302};
        for(String kind:new String[]{"fp32","fp16"})for(int id:ids) {
            File file=new File(root,"lsfg27/"+kind+"/"+id+".spv");
            if(!file.isFile()||file.length()<20)return false;
        }
        return true;
    }
    static byte[] merge(byte[] raw,State state,boolean clearEffects)throws IOException {
        String text=decode(raw);boolean bom=text.startsWith("\ufeff");
        if(bom)text=text.substring(1);
        String newline=text.contains("\r\n")?"\r\n":text.contains("\n")?"\n":text.contains("\r")?"\r":"\n";
        List<String> all=lines(text);Map<String,Integer> desired=new LinkedHashMap<>();
        desired.put("fsr3_mode",0);desired.put("framegen26",0);desired.put("framegen27",0);desired.put("fg_engine39",state.engine);desired.put("lsfg27_flow",state.flow);desired.put("lsfg30_multiplier",state.multiplier);desired.put("lsfg30_refresh",state.refresh);
        if(state.enabled&&clearEffects){desired.put("fsr144",0);desired.put("screen_filter144",0);desired.put("spatial34",0);}
        Map<String,Boolean> found=new LinkedHashMap<>();String current="";int end=-1;
        for(int i=0;i<all.size();i++) {
            String line=all.get(i),next=section(line);
            if(next!=null){if(current.equals("graphics"))end=i;current=next;continue;}
            if(!current.equals("graphics"))continue;
            int eq=line.indexOf('=');if(eq<=0)continue;
            String key=line.substring(0,eq).trim().toLowerCase(Locale.ROOT);
            if(!desired.containsKey(key))continue;
            int suffix=line.indexOf(';',eq+1),hash=line.indexOf('#',eq+1);
            if(hash>=0&&(suffix<0||hash<suffix))suffix=hash;
            String ending=line.endsWith("\r\n")?"\r\n":line.endsWith("\n")?"\n":line.endsWith("\r")?"\r":"";
            String comment=suffix<0?"":" "+line.substring(suffix,line.length()-ending.length());
            all.set(i,line.substring(0,eq+1)+desired.get(key)+comment+ending);found.put(key,true);
        }
        if(current.equals("graphics"))end=all.size();
        List<String> missing=new ArrayList<>();
        for(Map.Entry<String,Integer> e:desired.entrySet())if(!found.containsKey(e.getKey()))missing.add(e.getKey()+"="+e.getValue()+newline);
        if(!missing.isEmpty()) {
            if(end<0) {
                if(!all.isEmpty()&&!all.get(all.size()-1).endsWith("\n")&&!all.get(all.size()-1).endsWith("\r"))all.set(all.size()-1,all.get(all.size()-1)+newline);
                all.add((all.isEmpty()?"":newline)+"[graphics]"+newline);all.addAll(missing);
            }else {
                if(end>0&&!all.get(end-1).endsWith("\n")&&!all.get(end-1).endsWith("\r"))all.set(end-1,all.get(end-1)+newline);
                all.addAll(end,missing);
            }
        }
        StringBuilder result=new StringBuilder(bom?"\ufeff":"");for(String line:all)result.append(line);
        return result.toString().getBytes(StandardCharsets.UTF_8);
    }
    /** Called only while the separate :game process is stopped. */
    static synchronized void save(File root,State state,boolean clearEffects)throws IOException {
        if(!root.isDirectory()&&!root.mkdirs())throw new IOException("Cannot create game data folder");
        if(state.enabled&&!clearEffects&&effectsConflict(root))throw new IOException("Disable FSR1, spatial upscalers and screen filters before using frame generation");
        File target=new File(root,"settings.ini");byte[] before=readRaw(target),after=merge(before,state,clearEffects);
        if(Arrays.equals(before,after))return;
        if(target.exists()) {
            File folder=new File(root,"settings-backups");
            if(!folder.isDirectory()&&!folder.mkdirs())throw new IOException("Cannot create settings backup folder");
            Files.write(new File(folder,"settings-before-lsfg27-"+System.currentTimeMillis()+"-"+UUID.randomUUID()+".ini").toPath(),before);
        }
        Path temporary=Files.createTempFile(root.toPath(),"settings-lsfg27-",".tmp");
        try {
            try(FileOutputStream out=new FileOutputStream(temporary.toFile())){out.write(after);out.getFD().sync();}
            if(!Arrays.equals(before,readRaw(target)))throw new IOException("settings.ini changed; try applying again");
            try{Files.move(temporary,target.toPath(),StandardCopyOption.REPLACE_EXISTING,StandardCopyOption.ATOMIC_MOVE);}
            catch(AtomicMoveNotSupportedException unsupported){Files.move(temporary,target.toPath(),StandardCopyOption.REPLACE_EXISTING);}
        }finally{Files.deleteIfExists(temporary);}
        State stored=read(root);
        if(stored.engine!=state.engine||stored.flow!=state.flow||stored.multiplier!=state.multiplier||stored.refresh!=state.refresh)
            throw new IOException("Could not verify saved frame generation options; apply again before starting the game");
    }
}
