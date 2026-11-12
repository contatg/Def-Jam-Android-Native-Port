// SPDX-License-Identifier: GPL-3.0-or-later
package org.defjamffny.android;

import android.content.Context;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/** Separate hook set: never replaces the native Def Jam driver's libraries. */
final class HakuXDriver36 {
    private static final String[] HOOKS={"libmain_hook.so","libfile_redirect_hook.so","libgsl_alloc_hook.so","libkgsl_context_hook.so","libhook_impl.so"};
    static String specification(Context context,String library) throws IOException {
        if(library==null||library.isEmpty())return "";
        if(!library.equals(new File(library).getName())||library.contains("|"))throw new IOException("Invalid driver name");
        File driver=new File(context.getFilesDir(),"gpu_driver/current");
        if(!new File(driver,library).isFile())throw new IOException("Imported driver missing");
        File hooks=new File(context.getCodeCacheDir(),"hakux36-hooks-v1");
        if(!hooks.isDirectory()&&!hooks.mkdirs())throw new IOException("Cannot create hakuX hook directory");
        for(String name:HOOKS){
            File output=new File(hooks,name),temp=new File(hooks,name+".part");
            try(InputStream in=context.getAssets().open("hakux36/hooks/"+name);FileOutputStream out=new FileOutputStream(temp)){
                byte[] buffer=new byte[16384];int n,total=0;
                while((n=in.read(buffer))!=-1){total+=n;if(total>2*1024*1024)throw new IOException("Hook too large");out.write(buffer,0,n);}
                out.getFD().sync();
            }catch(IOException e){temp.delete();throw e;}
            if(output.exists()&&!output.delete()){temp.delete();throw new IOException("Cannot replace old hook");}
            if(!temp.renameTo(output)){temp.delete();throw new IOException("Cannot finalize hook");}
            output.setReadable(true,true);output.setExecutable(true,true);
        }
        return driver.getAbsolutePath()+File.separator+"|"+library+"|"+hooks.getAbsolutePath()+File.separator;
    }
    private HakuXDriver36(){}
}
