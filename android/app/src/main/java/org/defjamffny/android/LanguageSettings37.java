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
import java.util.List;
import java.util.Locale;

/** Edits just one language key, preserving other settings and comments. */
final class LanguageSettings37 {
    static byte[] read(File path) throws IOException {
        if (!path.exists()) return new byte[0];
        if (!path.isFile() || path.length() > 1024 * 1024) throw new IOException("Invalid settings.ini");
        return Files.readAllBytes(path.toPath());
    }
    private static String decode(byte[] raw) throws IOException {
        return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw)).toString();
    }
    private static List<String> lines(String s) {
        List<String> list=new ArrayList<>();int begin=0;
        for(int i=0;i<s.length();i++) if(s.charAt(i)=='\r'||s.charAt(i)=='\n') {
            if(s.charAt(i)=='\r'&&i+1<s.length()&&s.charAt(i+1)=='\n')i++;
            list.add(s.substring(begin,i+1));begin=i+1;
        }
        if(begin<s.length())list.add(s.substring(begin));return list;
    }
    private static String clean(String s) {
        int a=s.indexOf(';'),b=s.indexOf('#'),n=a<0?b:b<0?a:Math.min(a,b);
        return (n<0?s:s.substring(0,n)).trim();
    }
    private static String section(String line) {
        line=clean(line);
        return line.startsWith("[")&&line.endsWith("]") ? line.substring(1,line.length()-1).trim().toLowerCase(Locale.ROOT):null;
    }
    static String value(File root,String key,String fallback) throws IOException {
        String text=decode(read(new File(root,"settings.ini")));if(text.startsWith("\ufeff"))text=text.substring(1);
        String section="",result=fallback;
        for(String line:lines(text)) {
            String sec=section(line);if(sec!=null){section=sec;continue;}
            if(!section.equals("language"))continue;
            String t=clean(line);int eq=t.indexOf('=');
            if(eq>0&&t.substring(0,eq).trim().equalsIgnoreCase(key))result=t.substring(eq+1).trim();
        }
        return result;
    }
    static byte[] merge(byte[] raw,String key,String value) throws IOException {
        if(!key.equals("pack")&&!key.equals("story_subtitles"))throw new IOException("Unsupported language setting");
        if(value.indexOf('\n')>=0||value.indexOf('\r')>=0||value.indexOf(';')>=0||value.indexOf('#')>=0)
            throw new IOException("Invalid language value");
        String text=decode(raw);boolean bom=text.startsWith("\ufeff");if(bom)text=text.substring(1);
        String nl=text.contains("\r\n")?"\r\n":text.contains("\n")?"\n":text.contains("\r")?"\r":"\n";
        List<String> all=lines(text);String sec="";int end=-1;boolean found=false;
        for(int i=0;i<all.size();i++) {
            String line=all.get(i),next=section(line);
            if(next!=null){if(sec.equals("language"))end=i;sec=next;continue;}
            if(!sec.equals("language"))continue;
            int eq=line.indexOf('=');if(eq<=0||!line.substring(0,eq).trim().equalsIgnoreCase(key))continue;
            int c=line.indexOf(';',eq+1),h=line.indexOf('#',eq+1);if(h>=0&&(c<0||h<c))c=h;
            String ending=line.endsWith("\r\n")?"\r\n":line.endsWith("\n")?"\n":line.endsWith("\r")?"\r":"";
            String comment=c<0?"":line.substring(c,line.length()-ending.length()).trim();
            // The native INI parser accepts whole-line comments, not trailing comments.
            String suffix=comment.isEmpty()?ending:(ending.isEmpty()?nl:ending)+comment+ending;
            all.set(i,line.substring(0,eq+1)+value+suffix);found=true;
        }
        if(sec.equals("language"))end=all.size();
        if(!found) {
            if(end<0){
                if(!all.isEmpty()&&!all.get(all.size()-1).endsWith("\n")&&!all.get(all.size()-1).endsWith("\r"))all.set(all.size()-1,all.get(all.size()-1)+nl);
                all.add((all.isEmpty()?"":nl)+"[language]"+nl);end=all.size();
            }else if(end>0&&!all.get(end-1).endsWith("\n")&&!all.get(end-1).endsWith("\r"))all.set(end-1,all.get(end-1)+nl);
            all.add(end,key+"="+value+nl);
        }
        StringBuilder out=new StringBuilder(bom?"\ufeff":"");for(String line:all)out.append(line);
        return out.toString().getBytes(StandardCharsets.UTF_8);
    }
    static synchronized void save(File root,String key,String value) throws IOException {
        if(!root.isDirectory()&&!root.mkdirs())throw new IOException("Cannot create game folder");
        File target=new File(root,"settings.ini");byte[] before=read(target),after=merge(before,key,value);
        if(Arrays.equals(before,after))return;
        Path tmp=Files.createTempFile(root.toPath(),"language37-",".tmp");
        try {
            try(FileOutputStream out=new FileOutputStream(tmp.toFile())){out.write(after);out.getFD().sync();}
            if(!Arrays.equals(before,read(target)))throw new IOException("settings.ini changed while saving; try again");
            try{Files.move(tmp,target.toPath(),StandardCopyOption.REPLACE_EXISTING,StandardCopyOption.ATOMIC_MOVE);}
            catch(AtomicMoveNotSupportedException e){Files.move(tmp,target.toPath(),StandardCopyOption.REPLACE_EXISTING);}
        } finally {Files.deleteIfExists(tmp);}
        if(!value.equals(value(root,key,"")))throw new IOException("Saved language setting could not be verified");
    }
    private LanguageSettings37() { }
}
