package org.defjamffny.android;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Bounded reader for the owner's BIG/RefPack/SHPX archives. No game data. */
final class OnlineArtArchive {
    static final int LIMIT=16*1024*1024;
    static final class Entry {
        final String name;final long offset;final int size;
        Entry(String n,long o,int s){name=n;offset=o;size=s;}
    }
    static final class Image {
        final int width,height;final int[] argb;
        Image(int w,int h,int[] p){width=w;height=h;argb=p;}
    }
    static void require(boolean ok)throws IOException{if(!ok)throw new IOException("Invalid or unsupported online artwork archive");}
    static int u(byte[] b,int p)throws IOException{require(p>=0&&p<b.length);return b[p]&255;}
    static int le16(byte[] b,int p)throws IOException{return u(b,p)|(u(b,p+1)<<8);}
    static long le32(byte[] b,int p)throws IOException{return (long)le16(b,p)|((long)le16(b,p+2)<<16);}
    static long be32(byte[] b,int p)throws IOException{return ((long)le16be(b,p)<<16)|le16be(b,p+2);}
    static int le16be(byte[] b,int p)throws IOException{return (u(b,p)<<8)|u(b,p+1);}
    static List<Entry> directory(RandomAccessFile f)throws IOException{
        f.seek(0);byte[] h=new byte[16];f.readFully(h);
        String magic=new String(h,0,4,StandardCharsets.US_ASCII);
        require(magic.equals("BIGF")||magic.equals("BIG4"));
        long count=be32(h,8),end=be32(h,12);require(count<=20000&&end>=16&&end<=f.length());
        List<Entry> out=new ArrayList<>();
        for(long i=0;i<count;i++){
            require(f.getFilePointer()+8<=end);
            long off=Integer.toUnsignedLong(f.readInt()),size=Integer.toUnsignedLong(f.readInt());
            StringBuilder n=new StringBuilder();int c;
            do{require(f.getFilePointer()<end&&n.length()<1024);c=f.readUnsignedByte();if(c!=0)n.append((char)c);}while(c!=0);
            require(size<=Integer.MAX_VALUE&&off>=end&&off+size<=f.length());
            out.add(new Entry(n.toString(),off,(int)size));
        }return out;
    }
    static byte[] read(RandomAccessFile f,Entry e)throws IOException{
        require(e.size<=LIMIT);byte[] b=new byte[e.size];f.seek(e.offset);f.readFully(b);return b;
    }
    static Map<String,byte[]> inner(byte[] b)throws IOException{
        b=refpack(b);require(b.length>=16);
        String magic=new String(b,0,4,StandardCharsets.US_ASCII);
        require(magic.equals("BIGF")||magic.equals("BIG4"));
        long count=be32(b,8),end=be32(b,12);require(count<=20000&&end>=16&&end<=b.length);
        int p=16;Map<String,byte[]> out=new LinkedHashMap<>();
        for(int i=0;i<count;i++){
            long off=be32(b,p),size=be32(b,p+4);p+=8;int start=p;
            while(p<end&&p-start<1024&&u(b,p)!=0)p++;
            require(p<end&&p-start<1024&&off>=end&&off+size<=b.length);
            String name=new String(b,start,p-start,StandardCharsets.ISO_8859_1);p++;
            out.put(name,Arrays.copyOfRange(b,(int)off,(int)(off+size)));
        }return out;
    }
    static byte[] refpack(byte[] b)throws IOException{
        if(b.length<2||(((u(b,0)<<8)|u(b,1))&0x3eff)!=0x10fb)return b;
        int sig=(u(b,0)<<8)|u(b,1),step=(sig&0x8000)!=0?4:3,p=2+((sig&0x100)!=0?step:0);
        long size=0;for(int k=0;k<step;k++)size=(size<<8)|u(b,p++);
        require(size<=LIMIT);byte[] out=new byte[(int)size];int n=0;boolean stopped=false;
        while(p<b.length){
            int b0=u(b,p++),run,len=0,dist=0;
            if(b0<0x80){int b1=u(b,p++);run=b0&3;len=((b0&0x1c)>>2)+3;dist=((b0&0x60)<<3)+b1+1;}
            else if(b0<0xc0){int b1=u(b,p++),b2=u(b,p++);run=(b1>>6)&3;len=(b0&0x3f)+4;dist=((b1&0x3f)<<8)+b2+1;}
            else if(b0<0xe0){int b1=u(b,p++),b2=u(b,p++),b3=u(b,p++);run=b0&3;len=((b0&12)<<6)+b3+5;dist=((b0&16)<<12)+(b1<<8)+b2+1;}
            else run=b0<0xfc?((b0&31)<<2)+4:b0&3;
            require(p+run<=b.length&&n+run+len<=out.length);
            System.arraycopy(b,p,out,n,run);p+=run;n+=run;
            require(len==0||(dist>0&&dist<=n));
            for(int k=0;k<len;k++){out[n]=out[n-dist];n++;}
            if(b0>=0xfc){stopped=true;break;}
        }
        require(stopped&&n==out.length);return out;
    }
    static int morton(int x,int y,int w,int h){
        int out=0,d=0;
        for(int bit=0;(1<<bit)<w||(1<<bit)<h;bit++){
            if((1<<bit)<w)out|=((x>>bit)&1)<<d++;
            if((1<<bit)<h)out|=((y>>bit)&1)<<d++;
        }return out;
    }
    static int bgra(byte[] b,int p)throws IOException{return (u(b,p+3)<<24)|(u(b,p+2)<<16)|(u(b,p+1)<<8)|u(b,p);}
    static int rgb565(int c){int r=(c>>11)&31,g=(c>>5)&63,b=c&31;return 0xff000000|((r<<3|r>>2)<<16)|((g<<2|g>>4)<<8)|(b<<3|b>>2);}
    static int blend(int a,int b,int wa,int wb,int div){
        int v=0xff000000;for(int shift=0;shift<=16;shift+=8)v|=((((a>>shift)&255)*wa+((b>>shift)&255)*wb)/div)<<shift;return v;
    }
    static Image decode(byte[] b,int offset)throws IOException{
        require(offset>=0&&offset+16<=b.length);
        int kind=u(b,offset),w=le16(b,offset+4),h=le16(b,offset+6);long flags=le32(b,offset+12);
        require(w>0&&h>0&&w<=2048&&h<=2048&&(flags>>>28)==0);
        long start=offset+((flags&0x1000)!=0?le32(b,offset+16):16);
        require(start>=offset+16&&start<=b.length);int data=(int)start;int[] pixels=new int[w*h];
        if(kind==0x7d||kind==0x7b){
            int bpp=kind==0x7d?4:1;require(start+(long)w*h*bpp<=b.length);
            int palette=-1,colors=0;
            if(kind==0x7b){int pos=offset;Set<Integer> seen=new HashSet<>();
                while(seen.add(pos)&&seen.size()<=64){int delta=u(b,pos+1)|(u(b,pos+2)<<8)|(u(b,pos+3)<<16);if(delta==0)break;pos+=delta;require(pos>=0&&pos+16<=b.length);
                    if(u(b,pos)==0x2a){colors=le16(b,pos+4);require(colors>0&&colors<=256&&pos+16+colors*4<=b.length);palette=pos+16;break;}}
                require(palette>=0);
            }
            for(int y=0;y<h;y++)for(int x=0;x<w;x++){
                int i=(flags&0x2000)!=0?morton(x,y,w,h):y*w+x;require(i>=0&&i<w*h);
                if(kind==0x7d)pixels[y*w+x]=bgra(b,data+i*4);
                else{int idx=u(b,data+i);require(idx<colors);pixels[y*w+x]=bgra(b,palette+idx*4);}
            }
        }else{
            require(kind==0x60||kind==0x61||kind==0x62);int bytes=kind==0x60?8:16;
            require(start+(long)((w+3)/4)*((h+3)/4)*bytes<=b.length);
            for(int by=0;by<h;by+=4)for(int bx=0;bx<w;bx+=4){
                int alpha=data;int color=data+(kind==0x60?0:8),c0=le16(b,color),c1=le16(b,color+2);
                int[] c={rgb565(c0),rgb565(c1),0,0};
                if(c0>c1||kind!=0x60){c[2]=blend(c[0],c[1],2,1,3);c[3]=blend(c[0],c[1],1,2,3);}
                else{c[2]=blend(c[0],c[1],1,1,2);c[3]=0;}
                long bits=le32(b,color+4),abits=0;int[] av=new int[8];
                if(kind==0x62){av[0]=u(b,alpha);av[1]=u(b,alpha+1);for(int i=0;i<6;i++)abits|=(long)u(b,alpha+2+i)<<(8*i);
                    if(av[0]>av[1])for(int i=1;i<=6;i++)av[i+1]=((7-i)*av[0]+i*av[1])/7;
                    else{for(int i=1;i<=4;i++)av[i+1]=((5-i)*av[0]+i*av[1])/5;av[6]=0;av[7]=255;}}
                for(int i=0;i<16;i++){
                    int v=c[(int)(bits>>(2*i))&3];
                    if(kind==0x61)v=(v&0xffffff)|((((u(b,alpha+i/2)>>(4*(i&1)))&15)*17)<<24);
                    if(kind==0x62)v=(v&0xffffff)|(av[(int)(abits>>(3*i))&7]<<24);
                    int x=bx+(i&3),y=by+(i>>2);if(x<w&&y<h)pixels[y*w+x]=v;
                }data+=bytes;
            }
        }return new Image(w,h,pixels);
    }
    static Image image(byte[] entry,String wanted)throws IOException{
        for(Map.Entry<String,byte[]> part:inner(entry).entrySet())if(part.getKey().toLowerCase(Locale.ROOT).endsWith(".xsh")){
            byte[] b=part.getValue();require(b.length>=16&&new String(b,0,4,StandardCharsets.US_ASCII).equals("SHPX")&&le32(b,4)==b.length);
            long count=le32(b,8);require(count<=65536&&16+count*8<=b.length);
            for(int i=0;i<count;i++){
                int pos=16+i*8;String name=new String(b,pos,4,StandardCharsets.ISO_8859_1).replace("\0","").trim();
                long off=le32(b,pos+4);require(off<=Integer.MAX_VALUE);
                if((wanted==null||wanted.equals(name))&&u(b,(int)off)!=0x2a)return decode(b,(int)off);
            }
        }throw new IOException("Online image not present in archive");
    }
}
