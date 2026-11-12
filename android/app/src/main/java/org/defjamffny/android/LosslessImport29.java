package org.defjamffny.android;

import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;

/** Reads PE resources as data. Never loads or executes Windows code. */
final class LosslessImport29 {
    private static final int[] IDS={255,256,280,281,282,283,284,285,286,287,288,289,290,291,292,293,294,295,296,297,298,299,300,301,302};
    private final byte[] data;private final ByteBuffer b;
    private final List<int[]> sections=new ArrayList<>();private int resource,size;
    private final Map<Integer,byte[]> shaders=new HashMap<>();
    private LosslessImport29(byte[] bytes){data=bytes;b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);}
    private void bounds(long p,long n)throws IOException{if(p<0||n<0||p+n>data.length)throw new IOException("PE truncado");}
    private int u16(int p)throws IOException{bounds(p,2);return b.getShort(p)&65535;}
    private int i32(int p)throws IOException{bounds(p,4);return b.getInt(p);}
    private int offset(long rva,long n)throws IOException{
        for(int[] s:sections){long va=Integer.toUnsignedLong(s[0]),raw=Integer.toUnsignedLong(s[1]),ptr=Integer.toUnsignedLong(s[2]);
            if(rva>=va&&rva+n<=va+raw){long p=ptr+rva-va;bounds(p,n);return (int)p;}}
        throw new IOException("RVA fora das seções PE");
    }
    private int entry(int pos,int n)throws IOException{if(pos<0||n<0||(long)pos+n>size)throw new IOException("Recurso PE inválido");return resource+pos;}
    private void walk(int at,List<Integer> path)throws IOException{
        if(path.size()>3)throw new IOException("Árvore de recursos inválida");
        int p=entry(at,16),total=u16(p+12)+u16(p+14);if(total>4096)throw new IOException("Recursos demais");entry(at+16,total*8);
        for(int i=0;i<total;i++){
            int id=i32(p+16+i*8),target=i32(p+20+i*8);if(id<0)continue;
            if(target<0){List<Integer> next=new ArrayList<>(path);next.add(id);walk(target&0x7fffffff,next);}
            else if(path.size()==2&&path.get(0)==10){int q=entry(target,16),len=i32(q+4);if(len<20||len>2*1024*1024)throw new IOException("Tamanho de shader inválido");
                int start=offset(Integer.toUnsignedLong(i32(q)),len);byte[] blob=Arrays.copyOfRange(data,start,start+len);
                byte[] previous=shaders.put(path.get(1),blob);if(previous!=null&&!Arrays.equals(previous,blob))throw new IOException("Recurso ambíguo entre idiomas");}
        }
    }
    private Map<Integer,byte[]> resources()throws IOException{
        if(u16(0)!=0x5a4d)throw new IOException("Não é uma DLL PE/MZ");int pe=i32(0x3c);if(i32(pe)!=0x4550)throw new IOException("Assinatura PE inválida");
        int count=u16(pe+6),opt=pe+24,length=u16(pe+20);if(count<1||count>96||length<136)throw new IOException("Cabeçalho PE inválido");
        bounds(opt,length);int magic=u16(opt),directory=opt+(magic==0x20b?112:magic==0x10b?96:0);if(directory==opt||i32(directory-4)<3)throw new IOException("DLL sem recursos");
        for(int i=0;i<count;i++){int p=opt+length+40*i;bounds(p,40);sections.add(new int[]{i32(p+12),i32(p+16),i32(p+20)});}
        size=i32(directory+20);if(size<16||size>64*1024*1024)throw new IOException("Seção de recursos inválida");resource=offset(Integer.toUnsignedLong(i32(directory+16)),size);walk(0,new ArrayList<>());return shaders;
    }
    static byte[] dense(byte[] source,boolean fp32)throws IOException{
        if(source.length<20||source.length%4!=0)throw new IOException("SPIR-V truncado");
        IntBuffer ib=ByteBuffer.wrap(source).order(ByteOrder.LITTLE_ENDIAN).asIntBuffer();int[] w=new int[ib.remaining()];ib.get(w);
        if(w[0]!=0x07230203||Integer.toUnsignedLong(w[1])>0x10300||w[3]<1||w[3]>1000000||w[4]!=0)throw new IOException("SPIR-V incompatível com Vulkan 1.1");
        Map<Integer,int[]> ptr=new HashMap<>(),vars=new HashMap<>(),bindings=new HashMap<>(),sets=new HashMap<>();Map<Integer,Integer> types=new HashMap<>();boolean memory=false;
        for(int i=5;i<w.length;){int n=w[i]>>>16,op=w[i]&65535;if(n<1||(long)i+n>w.length)throw new IOException("Instrução SPIR-V inválida");
            if(op==17){if(n!=2)throw new IOException("Capability inválida");if(fp32&&w[i+1]==9)throw new IOException("Shader FP32 requer Float16");}
            else if(op==14){if(n!=3||w[i+1]!=0||w[i+2]!=1)throw new IOException("Memory model não suportado");memory=true;}
            else if(op==25){if(n<9)throw new IOException("OpTypeImage inválido");types.put(w[i+1],w[i+7]==2?3:2);}
            else if(op==26){if(n!=2)throw new IOException("OpTypeSampler inválido");types.put(w[i+1],1);}
            else if(op==30){if(n<2)throw new IOException("OpTypeStruct inválido");types.put(w[i+1],0);}
            else if(op==32){if(n!=4)throw new IOException("OpTypePointer inválido");ptr.put(w[i+1],new int[]{w[i+2],w[i+3]});}
            else if(op==59){if(n<4)throw new IOException("OpVariable inválido");vars.put(w[i+2],new int[]{w[i+1],w[i+3]});}
            else if(op==71&&n>=4){if(w[i+2]==33)bindings.put(w[i+1],new int[]{w[i+3],i+3});else if(w[i+2]==34)sets.put(w[i+1],new int[]{w[i+3],i+3});}i+=n;
        }
        if(!memory||bindings.isEmpty())throw new IOException("Shader sem memory model ou bindings");
        List<int[]> sites=new ArrayList<>();Set<Long> unique=new HashSet<>();
        for(Map.Entry<Integer,int[]> e:bindings.entrySet()){
            int[] v=vars.get(e.getKey()),set=sets.get(e.getKey());if(v==null||set==null||set[0]!=0)throw new IOException("Descriptor inválido");
            int[] p=ptr.get(v[0]);Integer kind=p==null?null:types.get(p[1]);if(kind==null||(kind==0&&v[1]!=2))throw new IOException("Tipo de descriptor não suportado");
            long key=((long)kind<<32)|Integer.toUnsignedLong(e.getValue()[0]);if(!unique.add(key))throw new IOException("Binding ambíguo");sites.add(new int[]{kind,e.getValue()[0],e.getValue()[1]});
        }
        sites.sort((a,c)->{int v=Integer.compare(a[0],c[0]);return v!=0?v:Integer.compareUnsigned(a[1],c[1]);});
        for(int i=0;i<sites.size();i++)w[sites.get(i)[2]]=i;
        ByteBuffer out=ByteBuffer.allocate(source.length).order(ByteOrder.LITTLE_ENDIAN);out.asIntBuffer().put(w);return out.array();
    }
    static void extract(InputStream input,File target)throws IOException{
        if(target.exists())throw new IOException("Destino já existe");ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] chunk=new byte[65536];int count;
        while((count=input.read(chunk))!=-1){if(out.size()+count>64*1024*1024)throw new IOException("DLL maior que 64 MiB");out.write(chunk,0,count);}
        Map<Integer,byte[]> resources=new LosslessImport29(out.toByteArray()).resources();
        if(!target.mkdirs())throw new IOException("Não foi possível preparar a importação");
        try{for(int precision=0;precision<2;precision++){
            File folder=new File(target,precision==0?"fp32":"fp16");if(!folder.mkdir())throw new IOException("Falha na pasta de shaders");
            for(int id:IDS){byte[] data=resources.get(id+(precision==0?98:49));if(data==null)throw new IOException("Versão da DLL incompatível: recurso "+id+" ausente");
                byte[] code=dense(data,precision==0);try(FileOutputStream f=new FileOutputStream(new File(folder,id+".spv"))){f.write(code);f.getFD().sync();}}
        }}catch(IOException|RuntimeException error){delete(target);throw error;}
    }
    static void delete(File root){File[] files=root.listFiles();if(files!=null)for(File f:files)delete(f);root.delete();}
}
