package org.defjamffny.android;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

/** Shares only images created in this application's private cache, read-only. */
public final class XisoShareProvider34 extends ContentProvider {
    @Override public boolean onCreate(){return true;}
    static File directory(android.content.Context c){return new File(c.getExternalFilesDir(null),"hakux-cache34");}
    static Uri uri(android.content.Context c,File f){return new Uri.Builder().scheme("content").authority(c.getPackageName()+".xiso34").appendPath(f.getName()).build();}
    private File checked(Uri uri) throws FileNotFoundException {
        if(getContext()==null||uri.getPathSegments().size()!=1)throw new FileNotFoundException("Invalid image URI");
        if(getContext().getExternalFilesDir(null)==null)throw new FileNotFoundException("Storage unavailable");
        String name=uri.getLastPathSegment();
        if(name==null||!name.matches("DefJam_[a-f0-9]{32}\\.xiso\\.iso"))throw new FileNotFoundException("Invalid image name");
        try{File dir=directory(getContext()).getCanonicalFile(), file=new File(dir,name).getCanonicalFile();
            if(!dir.equals(file.getParentFile())||!file.isFile())throw new FileNotFoundException("Image not found");return file;
        }catch(IOException e){throw new FileNotFoundException("Image unavailable");}
    }
    @Override public ParcelFileDescriptor openFile(Uri uri,String mode) throws FileNotFoundException {
        if(!"r".equals(mode))throw new FileNotFoundException("Read-only");return ParcelFileDescriptor.open(checked(uri),ParcelFileDescriptor.MODE_READ_ONLY);
    }
    @Override public Cursor query(Uri uri,String[] projection,String selection,String[] args,String order){
        try{File f=checked(uri);String[] cols=projection!=null?projection:new String[]{OpenableColumns.DISPLAY_NAME,OpenableColumns.SIZE};
            MatrixCursor c=new MatrixCursor(cols,1);Object[] row=new Object[cols.length];for(int i=0;i<cols.length;i++){
                if(OpenableColumns.DISPLAY_NAME.equals(cols[i]))row[i]=f.getName();else if(OpenableColumns.SIZE.equals(cols[i]))row[i]=f.length();}
            c.addRow(row);return c;
        }catch(FileNotFoundException e){return null;}
    }
    @Override public String getType(Uri uri){return "application/octet-stream";}
    @Override public Uri insert(Uri uri,ContentValues values){throw new UnsupportedOperationException();}
    @Override public int delete(Uri uri,String selection,String[] args){throw new UnsupportedOperationException();}
    @Override public int update(Uri uri,ContentValues values,String selection,String[] args){throw new UnsupportedOperationException();}
}
