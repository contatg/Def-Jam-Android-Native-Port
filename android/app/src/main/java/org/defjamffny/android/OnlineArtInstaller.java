package org.defjamffny.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.media.MediaScannerConnection;
import android.util.Log;
import java.io.*;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.regex.*;

/** Extract locally on EVERY installation; never downloads or packages disc artwork. */
final class OnlineArtInstaller {
    private static final String TAG="ONLINE-ART126";
    private static final String[][] UI={
        {"online_poster","screens/intMain/support/mainMenuButtons.big","15"},
        {"online_icon","screens/intMain/support/mainMenuLogosIcons.big","17"},
        {"button_a","screens/xHead.big","15"},{"button_y","screens/xHead.big","17"},
        {"button_b","screens/xHead.big","19"},{"button_x","screens/xHead.big","21"},
        {"header_strip","screens/xHead.big","7"},{"menu_wall","screens/intMain/mainMenu.big","5"},
        {"menu_door","screens/intMain/mainMenu.big","53"},{"door_shutter","screens/intMain/mainMenu.big","18"},
        {"paper_sign","screens/options/options.big","4"},{"paper_edge","screens/options/options.big","7"},
        {"selector","screens/battle/cmtype.big","10"},{"label_red","screens/options/options.big","26"},
        {"glow","screens/intMain/support/mainMenuButtons.big","77"}};
    private static final String[] FONTS={"TradeBld.ttf","TradeCon.ttf","SchwartzHand_Bold2.ttf","FERRO___.TTF","TradeGothicLTStd-BoldExt.ttf","AachenStd-Bold.ttf"};
    static File directory(Context c){return new File(c.getFilesDir(),"online-art");}
    static File resolve(File base,String relative)throws IOException{
        File cur=base;
        for(String part:relative.split("/")){
            File direct=new File(cur,part);if(direct.isFile()||direct.isDirectory()){cur=direct;continue;}
            File[] candidates=cur.listFiles();File found=null;
            if(candidates!=null)for(File f:candidates)if(f.getName().equalsIgnoreCase(part)){
                if(found!=null)throw new IOException("Ambiguous archive name: "+part);found=f;
            }
            if(found==null)throw new FileNotFoundException(relative);cur=found;
        }return cur;
    }
    private static void write(File f,byte[] bytes)throws IOException{
        File parent=f.getParentFile();if(!parent.isDirectory()&&!parent.mkdirs())throw new IOException("Cannot create artwork cache");
        try(FileOutputStream out=new FileOutputStream(f)){out.write(bytes);}
    }
    private static long rgbDiff(int a,int b){return Math.abs((a&255)-(b&255))+Math.abs(((a>>8)&255)-((b>>8)&255))+Math.abs(((a>>16)&255)-((b>>16)&255));}
    private static OnlineArtArchive.Image trim(OnlineArtArchive.Image im){
        int w=im.width,h=im.height,right=w,bottom=h;
        while(right>Math.max(1,w/2)){long d=0;for(int y=0;y<h;y++)d+=rgbDiff(im.argb[y*w+right-1],im.argb[y*w+right-2]);if(d>=3L*h)break;right--;}
        while(bottom>Math.max(1,h/2)){long d=0;for(int x=0;x<w;x++)d+=rgbDiff(im.argb[(bottom-1)*w+x],im.argb[(bottom-2)*w+x]);if(d>=3L*w)break;bottom--;}
        if(w==right&&h==bottom)return im;int[] p=new int[right*bottom];
        for(int y=0;y<bottom;y++)System.arraycopy(im.argb,y*w,p,y*right,right);
        return new OnlineArtArchive.Image(right,bottom,p);
    }
    private static void png(File root,String relative,OnlineArtArchive.Image im)throws IOException{
        File f=new File(root,relative);File parent=f.getParentFile();if(!parent.isDirectory()&&!parent.mkdirs())throw new IOException("Cannot create artwork cache");
        Bitmap bitmap=Bitmap.createBitmap(im.argb,im.width,im.height,Bitmap.Config.ARGB_8888);
        try(FileOutputStream out=new FileOutputStream(f)){if(!bitmap.compress(Bitmap.CompressFormat.PNG,100,out))throw new IOException("PNG encode failed");}
        finally{bitmap.recycle();}
    }
    private static void erasePrivate(File root,File f)throws IOException{
        if(!f.getCanonicalPath().startsWith(root.getCanonicalPath()+File.separator))throw new IOException("Invalid cache cleanup path");
        File[] children=f.listFiles();if(children!=null)for(File child:children)erasePrivate(root,child);
        if(f.exists()&&!f.delete())throw new IOException("Cannot remove old private cache");
    }
    static void hideLegacy(Context c,File data){
        try{File marker=new File(data,".nomedia");if(data.isDirectory()&&!marker.exists()){write(marker,new byte[0]);MediaScannerConnection.scanFile(c,new String[]{marker.getAbsolutePath()},null,null);}}
        catch(IOException|SecurityException ex){Log.w(TAG,"Could not hide game-data folder",ex);}
        File[] folders=data.listFiles();if(folders==null)return;
        for(File f:folders)if(f.isDirectory()&&(f.getName().equals("online-art")||f.getName().startsWith("online-art.backup-")||f.getName().startsWith("online-art.build118-"))){
            try{File marker=new File(f,".nomedia");if(!marker.exists()){write(marker,new byte[0]);MediaScannerConnection.scanFile(c,new String[]{marker.getAbsolutePath()},null,null);}}
            catch(IOException|SecurityException ex){Log.w(TAG,"Could not hide legacy artwork folder",ex);}
        }
    }
    static synchronized void ensure(Context context,File data)throws IOException{
        hideLegacy(context,data);
        File home=context.getFilesDir(),target=directory(context);
        try(RandomAccessFile lockFile=new RandomAccessFile(new File(home,"online-art126.lock"),"rw");FileLock lock=lockFile.getChannel().lock()){
            File interruptedBackup=new File(home,"online-art126-old");
            if(!target.exists()&&interruptedBackup.exists()&&!interruptedBackup.renameTo(target))throw new IOException("Cannot recover previous private artwork cache");
            File screens=resolve(new File(data,"game"),"screens/screens.viv");File fonts=null;
            try{fonts=resolve(new File(data,"game"),"fonts/fonts.viv");}catch(FileNotFoundException missing){Log.w(TAG,"Optional fonts archive is absent");}
            String stamp="126:"+screens.length()+":"+screens.lastModified()+":"+(fonts==null?"none":fonts.length()+":"+fonts.lastModified());
            File stampFile=new File(target,"complete126");
            if(stampFile.isFile())try(FileInputStream in=new FileInputStream(stampFile)){
                byte[] b=new byte[256];int n=in.read(b);
                if(n>0&&stamp.equals(new String(b,0,n,StandardCharsets.UTF_8))&&new File(target,"fighters").isDirectory())return;
            }
            File stage=new File(home,"online-art126-stage"),old=new File(home,"online-art126-old");
            if(stage.exists())erasePrivate(home,stage);if(old.exists())erasePrivate(home,old);
            if(!stage.mkdirs())throw new IOException("Cannot create private artwork cache");
            int portraits=0,pictures=0;boolean committed=false,moved=false;
            try{
                write(new File(stage,".nomedia"),new byte[0]);
                Pattern fighter=Pattern.compile("screens/portrait/p_CT/p_CT_(\\d+)\\.big",Pattern.CASE_INSENSITIVE);
                Pattern venue=Pattern.compile("screens/portrait/(vp|mVen)(\\d+)\\.big",Pattern.CASE_INSENSITIVE);
                Pattern mode=Pattern.compile("screens/match/2_type/mTypeLogo_(\\w+?)_e\\.big",Pattern.CASE_INSENSITIVE);
                try(RandomAccessFile f=new RandomAccessFile(screens,"r")){
                    for(OnlineArtArchive.Entry e:OnlineArtArchive.directory(f)){
                        String name=e.name.replace('\\','/');Matcher m=fighter.matcher(name),v=venue.matcher(name),modes=mode.matcher(name);
                        try{
                            if(m.matches()){
                                int id=Integer.parseInt(m.group(1))-1;OnlineArtArchive.require(id>=0&&id<256&&pictures<256);
                                png(stage,"fighters/"+id+".png",trim(OnlineArtArchive.image(OnlineArtArchive.read(f,e),null)));portraits++;pictures++;
                            }else if(v.matches()){
                                int id=Integer.parseInt(v.group(2))-1;OnlineArtArchive.require(id>=0&&id<256&&pictures<256);
                                png(stage,"venues/"+id+(v.group(1).equalsIgnoreCase("vp")?"":"_name")+".png",OnlineArtArchive.image(OnlineArtArchive.read(f,e),null));pictures++;
                            }else if(modes.matches()){
                                png(stage,"modes/"+modes.group(1)+".png",OnlineArtArchive.image(OnlineArtArchive.read(f,e),null));pictures++;
                            }else{
                                byte[] blob=null;
                                for(String[] ui:UI)if(name.equalsIgnoreCase(ui[1])){
                                    if(blob==null)blob=OnlineArtArchive.read(f,e);
                                    png(stage,"ui/"+ui[0]+".png",OnlineArtArchive.image(blob,ui[2]));pictures++;
                                }
                            }
                        }catch(IOException|IllegalArgumentException unsupported){Log.w(TAG,"Optional artwork skipped: "+name,unsupported);}
                    }
                }
                if(fonts!=null)try(RandomAccessFile f=new RandomAccessFile(fonts,"r")){
                    for(OnlineArtArchive.Entry e:OnlineArtArchive.directory(f))for(String font:FONTS)if(e.name.equalsIgnoreCase(font))
                        write(new File(stage,"fonts/"+font),OnlineArtArchive.refpack(OnlineArtArchive.read(f,e)));
                }catch(IOException unsupportedFont){Log.w(TAG,"Optional font archive could not be read",unsupportedFont);}
                if(portraits==0)throw new IOException("No fighter portraits could be extracted from screens.viv");
                write(new File(stage,"complete126"),stamp.getBytes(StandardCharsets.UTF_8));
                if(target.exists()){if(!target.renameTo(old))throw new IOException("Cannot preserve old artwork cache");moved=true;}
                if(!stage.renameTo(target))throw new IOException("Cannot publish artwork cache");committed=true;
                Log.i(TAG,"Prepared locally: "+portraits+" fighters, "+pictures+" images; private cache");
            }finally{
                if(!committed&&moved&&!old.renameTo(target))Log.e(TAG,"Previous private artwork cache preserved for recovery");
                if(stage.exists())erasePrivate(home,stage);
                if(committed&&old.exists())erasePrivate(home,old);
            }
        }
    }
}
