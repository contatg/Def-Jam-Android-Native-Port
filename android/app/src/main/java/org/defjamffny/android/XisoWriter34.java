package org.defjamffny.android;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Builds an Xbox XDVDFS disc image from the user's extracted files.
 * Independent of Android so the image format can be tested on a host. */
final class XisoWriter34 {
    static final int SECTOR=2048, ROOT_SECTOR=0x108;
    static final byte[] MAGIC="MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
    interface Progress { void update(long copied,long total) throws IOException; }
    static final class Node {
        final File file; final byte[] name; final boolean dir; final List<Node> children=new ArrayList<>();
        Node left,right,dirRoot; long size,sector,offset,tableBytes; Node(File f,byte[] n,boolean d){file=f;name=n;dir=d;}
    }
    private final File root; private final Progress progress; private long total,nextSector=ROOT_SECTOR;
    private int count;
    XisoWriter34(File root,Progress progress) throws IOException {this.root=root.getCanonicalFile();this.progress=progress;}
    String sourceStamp() throws IOException {
        count=0;
        try {
            java.security.MessageDigest digest=java.security.MessageDigest.getInstance("SHA-256");
            digest.update((byte)39);
            stamp(scan(root,new byte[0],0),digest);
            StringBuilder key=new StringBuilder(32);
            byte[] bytes=digest.digest();
            for(int i=0;i<16;i++)key.append(String.format(java.util.Locale.ROOT,"%02x",bytes[i]&255));
            return key.toString();
        } catch(java.security.NoSuchAlgorithmException e) { throw new IOException(e); }
    }
    private static void stamp(Node node,java.security.MessageDigest digest) {
        digest.update((byte)(node.dir?1:0));
        digest.update((byte)node.name.length);
        digest.update(node.name);
        java.nio.ByteBuffer meta=java.nio.ByteBuffer.allocate(20);
        meta.putLong(node.size).putLong(node.file.lastModified()).putInt(node.children.size());
        digest.update(meta.array());
        for(Node child:node.children)stamp(child,digest);
    }
    private static long sectors(long size){return (size+SECTOR-1)/SECTOR;}
    private static int fold(int b){return b>='a'&&b<='z'?b-32:b;}
    private static int compare(Node a,Node b){
        int n=Math.min(a.name.length,b.name.length);for(int i=0;i<n;i++){int x=fold(a.name[i]&255),y=fold(b.name[i]&255);if(x!=y)return x-y;}
        return a.name.length-b.name.length;
    }
    private Node scan(File f,byte[] name,int depth) throws IOException {
        if(depth>64||++count>150000)throw new IOException("Pasta muito grande ou profunda para criar a imagem.");
        File canonical=f.getCanonicalFile();
        if(!canonical.equals(f.getAbsoluteFile()))throw new IOException("Atalho/symlink não suportado dentro do jogo: "+f.getName());
        boolean dir=f.isDirectory();if(!dir&&!f.isFile())throw new IOException("Arquivo inválido: "+f.getName());
        Node node=new Node(f,name,dir);
        if(dir){
            File[] files=f.listFiles();if(files==null)throw new IOException("Sem acesso à pasta: "+f);
            for(File child:files){
                String s=child.getName();byte[] b=s.getBytes(StandardCharsets.UTF_8);
                if(b.length==0||b.length>255)throw new IOException("Nome inválido: "+s);
                for(byte value:b)if((value&255)<32||(value&255)>126)throw new IOException("Nome não ASCII não suportado na imagem Xbox: "+s);
                node.children.add(scan(child,b,depth+1));
            }
            node.children.sort(XisoWriter34::compare);
            for(int i=1;i<node.children.size();i++)if(compare(node.children.get(i-1),node.children.get(i))==0)throw new IOException("Nomes iguais sem diferenciar maiúsculas: "+node.children.get(i).file.getName());
            Node tree=tree(node.children,0,node.children.size());long[] pos={0};offsets(tree,pos);node.dirRoot=tree;
            node.tableBytes=pos[0]==0?SECTOR:pos[0];
        }else {node.size=f.length();if(node.size>0xffffffffL)throw new IOException("Arquivo maior que 4 GiB: "+f.getName());total=Math.addExact(total,node.size);}
        return node;
    }
    private static Node tree(List<Node> a,int lo,int hi){if(lo>=hi)return null;int mid=(lo+hi)>>>1;Node n=a.get(mid);n.left=tree(a,lo,mid);n.right=tree(a,mid+1,hi);return n;}
    private static void offsets(Node n,long[] p) throws IOException {
        if(n==null)return;long len=(14L+n.name.length+3)&~3L;
        if((p[0]%SECTOR)+len>SECTOR)p[0]=(p[0]+SECTOR-1)/SECTOR*SECTOR;
        n.offset=p[0];if(n.offset/4>0xffff)throw new IOException("Diretório Xbox excede o limite de endereçamento.");
        p[0]+=len;offsets(n.left,p);offsets(n.right,p);
    }
    private void planDirectories(Node d) throws IOException {
        d.sector=nextSector;nextSector+=sectors(d.tableBytes);
        for(Node n:d.children)if(n.dir)planDirectories(n);
        if(nextSector>0xffffffffL)throw new IOException("Imagem excede o limite de setores Xbox.");
    }
    private void planFiles(Node d) throws IOException {
        for(Node n:d.children)if(n.dir)planFiles(n);else{n.sector=nextSector;nextSector+=sectors(n.size);if(nextSector>0xffffffffL)throw new IOException("Imagem grande demais.");}
    }
    private static void le(byte[] out,int at,long n,int bytes){for(int i=0;i<bytes;i++)out[at+i]=(byte)(n>>>(8*i));}
    private static void records(Node n,byte[] out){
        if(n==null)return;int p=(int)n.offset;
        le(out,p,n.left==null?0:n.left.offset/4,2);le(out,p+2,n.right==null?0:n.right.offset/4,2);
        le(out,p+4,n.sector,4);le(out,p+8,n.dir?sectors(n.tableBytes)*SECTOR:n.size,4);
        out[p+12]=(byte)(n.dir?0x10:0x20);out[p+13]=(byte)n.name.length;System.arraycopy(n.name,0,out,p+14,n.name.length);
        records(n.left,out);records(n.right,out);
    }
    private static void writeDirs(RandomAccessFile out,Node d) throws IOException {
        byte[] table=new byte[(int)(sectors(d.tableBytes)*SECTOR)];Arrays.fill(table,(byte)0xff);records(d.dirRoot,table);
        out.seek(d.sector*SECTOR);out.write(table);for(Node n:d.children)if(n.dir)writeDirs(out,n);
    }
    private long copied;
    private void writeFiles(RandomAccessFile out,Node d,byte[] buffer) throws IOException {
        for(Node n:d.children){
            if(n.dir){writeFiles(out,n,buffer);continue;}
            if(progress!=null)progress.update(copied,total);
            out.seek(n.sector*SECTOR);long left=n.size;
            try(InputStream in=new BufferedInputStream(new FileInputStream(n.file),buffer.length)){
                while(left>0){int nread=in.read(buffer,0,(int)Math.min(buffer.length,left));if(nread<0)throw new IOException("Arquivo alterado durante cópia: "+n.file);out.write(buffer,0,nread);left-=nread;copied+=nread;if(progress!=null)progress.update(copied,total);}
                if(in.read()!=-1)throw new IOException("Arquivo cresceu durante cópia: "+n.file);
            }
        }
    }
    /** Creates a new output only; a failed/cancelled operation removes its partial file. */
    void write(File output) throws IOException {
        if(!root.isDirectory()||!new File(root,"default.xbe").isFile())throw new IOException("default.xbe não encontrado na pasta do jogo.");
        if(output.getCanonicalPath().startsWith(root.getCanonicalPath()+File.separator))throw new IOException("Destino temporário não pode ficar dentro da pasta do jogo.");
        Node tree=scan(root,new byte[0],0);planDirectories(tree);planFiles(tree);
        long bytes=((nextSector*SECTOR+65535)/65536)*65536;
        if(output.exists())throw new IOException("A imagem de destino já existe.");
        File parent=output.getParentFile();if(!parent.isDirectory()&&!parent.mkdirs())throw new IOException("Não foi possível criar a pasta temporária.");
        if(parent.getUsableSpace()<bytes+64L*1024*1024)throw new IOException("Espaço insuficiente: a imagem precisa de aproximadamente "+(bytes/(1024*1024))+" MB livres.");
        boolean complete=false;
        try(RandomAccessFile out=new RandomAccessFile(output,"rw")){
            out.setLength(bytes);byte[] desc=new byte[SECTOR];System.arraycopy(MAGIC,0,desc,0,20);le(desc,20,tree.sector,4);le(desc,24,tree.tableBytes,4);System.arraycopy(MAGIC,0,desc,SECTOR-20,20);
            out.seek(0x10000);out.write(desc);writeDirs(out,tree);writeFiles(out,tree,new byte[1024*1024]);out.getFD().sync();complete=true;
        }finally{if(!complete)output.delete();}
        if(progress!=null)progress.update(total,total);
    }
}
