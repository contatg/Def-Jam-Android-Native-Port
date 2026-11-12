package org.defjamffny.android;
import java.io.*;
import java.util.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
/** Structural delta only: all original movie bytes are read from the player's dump. */
final class GraphicsMenu147 {
 static final String BEFORE="30a830a976820e9547b6426e3ade524c30714ade052b02807b651270a4409dc3",AFTER="933eeb943430c5a066682b7907ed8497620ad95adab9c538b8a8f5d2d30df0ff";
 static final String ENTRY="screens/options/options.big";
 static final String[] OPS={"c:0:11592","h:390000005489","c:11598:11862","h:a879","c:23462:7606","h:6f477261706869637331343700000000677261706869637354657874000000006772617068696373506f7300475241504849435300000000506f727447726170686963732e4f70656e2829","c:25651:729","h:e40100003254769878563412ae12ae134eaf01a15c79000049129d0011000000a1000000947900005aae15af16b2a0763eb50017","c:26392:2569","h:a100005c79000075a100006c790000ae67a1007c790000a10000005c790000b505a275401dae78a10000006c7900004ea294a1887900004f","c:28961:35","h:500000009900000068","c:29005:31","h:48000000a212ae3fa10000005c7900001cb50017ae3cae3bae3aae38aebcae36ae35b509","c:29060:368","h:03000000360000005a","c:21665:27","h:0000af43ffffffff00000000000000386c79","c:20006:18","h:5b","c:22113:28","h:59b443ffffffff00000000000000387c790000ffffffff0000","c:12270:222","h:d488000014890000"};
 static String hash(byte[] b)throws IOException{try{StringBuilder s=new StringBuilder();for(byte v:MessageDigest.getInstance("SHA-256").digest(b))s.append(String.format(java.util.Locale.ROOT,"%02x",v&255));return s.toString();}catch(java.security.NoSuchAlgorithmException e){throw new IOException(e);}}
 static byte[] patch(byte[] input)throws IOException{
  String h=hash(input);if(h.equals(AFTER))return input;
  if(!h.equals(BEFORE))throw new IOException("Unsupported original Options movie; kept unchanged");
  ByteArrayOutputStream out=new ByteArrayOutputStream();
  for(String op:OPS){String[] p=op.split(":");if(p[0].equals("c")){int off=Integer.parseInt(p[1]),n=Integer.parseInt(p[2]);OnlineArtArchive.require(off>=0&&n>0&&off+n<=input.length);out.write(input,off,n);}else{String v=p[1];for(int i=0;i<v.length();i+=2)out.write(Integer.parseInt(v.substring(i,i+2),16));}}
  byte[] b=out.toByteArray();if(!hash(b).equals(AFTER))throw new IOException("Graphics movie checksum failed");return b;
 }
 static void word(byte[] b,int p,long v,boolean big){for(int i=0;i<4;i++)b[p+i]=(byte)(v>>((big?3-i:i)*8));}
 static byte[] inner(Map<String,byte[]> files)throws IOException{
  int header=16;for(String n:files.keySet())header+=9+n.getBytes(StandardCharsets.ISO_8859_1).length;
  int at=(header+127)&~127;for(byte[] b:files.values())at=((at+b.length+127)&~127);
  OnlineArtArchive.require(at<=OnlineArtArchive.LIMIT);byte[] out=new byte[at];System.arraycopy(new byte[]{66,73,71,70},0,out,0,4);word(out,4,at,false);word(out,8,files.size(),true);word(out,12,header,true);at=(header+127)&~127;int p=16;
  for(Map.Entry<String,byte[]> e:files.entrySet()){byte[] n=e.getKey().getBytes(StandardCharsets.ISO_8859_1),v=e.getValue();word(out,p,at,true);word(out,p+4,v.length,true);System.arraycopy(n,0,out,p+8,n.length);p+=9+n.length;System.arraycopy(v,0,out,at,v.length);at=(at+v.length+127)&~127;}return out;
 }
 static byte[] refpack(byte[] b)throws IOException{
  OnlineArtArchive.require(b.length<0x1000000);ByteArrayOutputStream o=new ByteArrayOutputStream();o.write(0x10);o.write(0xfb);o.write(b.length>>16);o.write(b.length>>8);o.write(b.length);int p=0;
  while(b.length-p>=112){o.write(0xfb);o.write(b,p,112);p+=112;}
  int r=b.length-p;if(r>=4){int k=(r-4)/4,n=k*4+4;o.write(0xe0+k);o.write(b,p,n);p+=n;}
  o.write(0xfc|b.length-p);o.write(b,p,b.length-p);return o.toByteArray();
 }
 static synchronized void ensure(File data)throws IOException{
  File original=OnlineArtInstaller.resolve(new File(data,"game"),"screens/screens.viv"),target=new File(data,"override/screens/screens.viv");File base=target.isFile()?target:original;
  byte[] header,entry;long length;int record=-1;
  try(RandomAccessFile in=new RandomAccessFile(base,"r")){
   length=in.length();List<OnlineArtArchive.Entry> list=OnlineArtArchive.directory(in);OnlineArtArchive.Entry found=null;for(OnlineArtArchive.Entry e:list)if(e.name.equalsIgnoreCase(ENTRY)){if(found!=null)throw new IOException("Duplicate Options entry");found=e;}if(found==null)throw new IOException("Original Options missing");
   Map<String,byte[]> files=OnlineArtArchive.inner(OnlineArtArchive.read(in,found));if(!hash(files.get("options.const")).equals("7ea2f652b56e0b4a839f8dd8ce41ec7b549afdf1f9feef0ce7cc8b81b20fa922"))throw new IOException("Unsupported Options constants");byte[] apt=files.get("options.apt");if(apt==null)throw new IOException("Original Options APT missing");byte[] patched=patch(apt);if(patched==apt)return;files.put("options.apt",patched);entry=refpack(inner(files));
   in.seek(0);byte[] first=new byte[16];in.readFully(first);long h=OnlineArtArchive.be32(first,12);OnlineArtArchive.require(h<=4*1024*1024);header=new byte[(int)h];in.seek(0);in.readFully(header);int p=16;for(int i=0;i<list.size();i++){int start=p;p+=8;int n=p;while(p<header.length&&header[p]!=0)p++;OnlineArtArchive.require(p<header.length);String name=new String(header,n,p-n,StandardCharsets.ISO_8859_1);if(name.equalsIgnoreCase(ENTRY))record=start;p++;}OnlineArtArchive.require(record>=0);
  }
  File folder=target.getParentFile();if(!folder.isDirectory()&&!folder.mkdirs())throw new IOException("Cannot create menu override directory");File tmp=File.createTempFile("graphics147-",".tmp",folder);boolean committed=false;File backup=null;
  try{
   long offset=(length+3)&~3L;OnlineArtArchive.require(offset+entry.length<0x100000000L);word(header,4,offset+entry.length,false);word(header,record,offset,true);word(header,record+4,entry.length,true);
   try(RandomAccessFile in=new RandomAccessFile(base,"r");RandomAccessFile out=new RandomAccessFile(tmp,"rw")){byte[] buf=new byte[1024*1024];int n;while((n=in.read(buf))!=-1)out.write(buf,0,n);out.seek(0);out.write(header);out.seek(length);while(out.getFilePointer()<offset)out.write(0);out.write(entry);out.getFD().sync();}
   // Read the staged archive before publishing; every other entry remains an exact byte copy.
   try(RandomAccessFile verify=new RandomAccessFile(tmp,"r")){boolean ok=false;for(OnlineArtArchive.Entry e:OnlineArtArchive.directory(verify))if(e.name.equalsIgnoreCase(ENTRY))ok=hash(OnlineArtArchive.inner(OnlineArtArchive.read(verify,e)).get("options.apt")).equals(AFTER);if(!ok)throw new IOException("Staged menu verification failed");}
   if(target.exists()){backup=new File(folder,"screens.viv.pre-graphics147-"+UUID.randomUUID());if(!target.renameTo(backup))throw new IOException("Cannot preserve previous menu override");}
   if(!tmp.renameTo(target)){if(backup!=null&&!backup.renameTo(target))throw new IOException("Restore menu backup: "+backup.getName());throw new IOException("Cannot install menu override");}committed=true;
  }finally{if(!committed)tmp.delete();}
 }
}
