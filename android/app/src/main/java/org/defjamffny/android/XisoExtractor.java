package org.defjamffny.android;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;

/** Bounded, read-only XDVDFS reader. No executable patching, downloads or disc bytes bundled.
 * Format references: XboxDev/extract-xiso and xboxdevwiki.net/XDVDFS.
 * This is an independent Java implementation, not a bundled extract-xiso executable. */
public final class XisoExtractor {
    public static final String SUPPORTED_XBE_SHA256 =
            "31cc0d11f7c656b1a6f337fdf7d9bbaac9df11c324a2fb881f7eb639d4be00c2";
    private static final byte[] MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
    private static final int SECTOR = 2048, MAX_ENTRIES = 100000;
    private static final long MAX_TABLE = 32L * 1024 * 1024, MAX_DIRECTORY_BYTES = 64L * 1024 * 1024;
    private static final long MAX_FILES_BYTES = 8L * 1024 * 1024 * 1024;
    public interface Progress { void update(String phase, long done, long total, String file); }
    private static final class Entry {
        String path; long offset, size; boolean directory;
        Entry(String p, long o, long s, boolean d) { path=p; offset=o; size=s; directory=d; }
    }
    private static final class Directory {
        String path; long sector, size; int depth;
        Directory(String p, long s, long n, int d) { path=p; sector=s; size=n; depth=d; }
    }
    private final FileChannel source;
    private final long imageSize;
    private final AtomicBoolean canceled;
    private final Progress progress;
    private long partition, totalBytes, directoryBytes;
    private final List<Entry> entries = new ArrayList<>();
    private final Set<String> paths = new HashSet<>();
    private final Set<Long> directorySectors = new HashSet<>();

    private XisoExtractor(FileChannel channel, AtomicBoolean cancel, Progress report) throws IOException {
        source=channel; imageSize=channel.size(); canceled=cancel; progress=report;
        if (imageSize < 0x10800) throw new IOException("Not an Xbox ISO/XISO.");
    }
    private void checkCanceled() throws IOException {
        if (canceled.get() || Thread.currentThread().isInterrupted()) throw new IOException("Import canceled.");
    }
    private ByteBuffer read(long offset, int size) throws IOException {
        checkCanceled();
        if (offset < 0 || size < 0 || offset > imageSize - size) throw new IOException("ISO is truncated or has invalid offsets.");
        ByteBuffer b=ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN);
        while (b.hasRemaining()) {
            checkCanceled();
            int n=source.read(b, offset+b.position());
            if (n <= 0) throw new IOException("Could not read ISO data.");
        }
        b.flip(); return b;
    }
    private long sectorOffset(long sector, long size) throws IOException {
        long offset=partition+sector*SECTOR;
        if (offset < partition || size < 0 || offset > imageSize-size)
            throw new IOException("ISO is truncated or has invalid offsets.");
        return offset;
    }
    private void scan() throws IOException {
        ByteBuffer descriptor=null;
        // XISO and standard Xbox full-disc layouts. Do not scan arbitrary payload bytes for magic.
        for (long base : new long[] {0, 0x18300000L, 0x0FD90000L}) {
            if (base+0x10800 > imageSize) continue;
            ByteBuffer b=read(base+0x10000, SECTOR);
            byte[] head=new byte[20], tail=new byte[20];
            b.get(head); b.position(0x7ec); b.get(tail);
            if (Arrays.equals(head,MAGIC) && Arrays.equals(tail,MAGIC)) { partition=base; descriptor=b; break; }
        }
        if (descriptor == null) throw new IOException("Not an Xbox ISO/XISO. Select an uncompressed original Xbox image.");
        long rootSector=Integer.toUnsignedLong(descriptor.getInt(20));
        long rootSize=Integer.toUnsignedLong(descriptor.getInt(24));
        if (rootSize == 0) throw new IOException("The ISO contains no game files.");
        ArrayDeque<Directory> pending=new ArrayDeque<>();
        pending.add(new Directory("",rootSector,rootSize,0));
        while (!pending.isEmpty()) {
            checkCanceled();
            Directory dir=pending.removeFirst();
            if (dir.depth > 64 || dir.size > MAX_TABLE || dir.size < 14
                    || (directoryBytes+=dir.size) > MAX_DIRECTORY_BYTES
                    || !directorySectors.add(dir.sector)) throw new IOException("Invalid ISO directory tree.");
            ByteBuffer table=read(sectorOffset(dir.sector,dir.size),(int)dir.size);
            ArrayDeque<Integer> nodes=new ArrayDeque<>(); Set<Integer> visited=new HashSet<>();
            nodes.push(0);
            while (!nodes.isEmpty()) {
                checkCanceled();
                int at=nodes.pop();
                if (at < 0 || at > table.limit()-14 || !visited.add(at)) throw new IOException("Invalid ISO directory tree.");
                int left=Short.toUnsignedInt(table.getShort(at)), right=Short.toUnsignedInt(table.getShort(at+2));
                if (left == 0xffff && right == 0xffff && at == 0) break; // empty padded table
                long sector=Integer.toUnsignedLong(table.getInt(at+4));
                long size=Integer.toUnsignedLong(table.getInt(at+8));
                int attributes=Byte.toUnsignedInt(table.get(at+12)), length=Byte.toUnsignedInt(table.get(at+13));
                if (length == 0 || at+14+length > table.limit()) throw new IOException("Invalid ISO filename.");
                byte[] bytes=new byte[length]; table.position(at+14); table.get(bytes);
                String name=new String(bytes,StandardCharsets.ISO_8859_1);
                if (name.equals(".") || name.equals("..") || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0
                        || name.indexOf(':') >= 0) throw new IOException("Unsafe ISO filename.");
                for (int i=0;i<name.length();i++) if (name.charAt(i) < 32 || name.charAt(i) == 127)
                    throw new IOException("Unsafe ISO filename.");
                if (dir.path.isEmpty() && name.equalsIgnoreCase("default.xbe")) name="default.xbe";
                String path=dir.path.isEmpty()?name:dir.path+"/"+name;
                if (path.length() > 2048 || !paths.add(path.toLowerCase(Locale.ROOT)) || entries.size() >= MAX_ENTRIES)
                    throw new IOException("Duplicate or excessive ISO paths.");
                boolean directory=(attributes&0x10)!=0;
                entries.add(new Entry(path,sectorOffset(sector,size),size,directory));
                if (directory) {
                    if (size > 0) pending.add(new Directory(path,sector,size,dir.depth+1));
                } else {
                    totalBytes+=size;
                    if (totalBytes > MAX_FILES_BYTES) throw new IOException("ISO contents exceed the supported size.");
                }
                if (left != 0) nodes.push(left*4);
                if (right != 0) nodes.push(right*4);
            }
        }
        for (String required:new String[] {"fighter.xml", "gamedata.xml", "gconfig.xml", "venue.xml",
                "assets/", "audio/", "fonts/", "motion/", "movies/", "screens/"}) {
            boolean wantDirectory=required.endsWith("/"), found=false;
            String name=wantDirectory?required.substring(0,required.length()-1):required;
            for (Entry entry:entries) if (entry.path.equalsIgnoreCase(name) && entry.directory==wantDirectory) { found=true; break; }
            if (!found) throw new IOException("The ISO is missing required game files or folders.");
        }
        Entry xbe=null;
        for (Entry e:entries) if (e.path.equals("default.xbe") && !e.directory) xbe=e;
        if (xbe == null || xbe.size < 4) throw new IOException("default.xbe is missing from the ISO root.");
        if (read(xbe.offset,4).getInt() != 0x48454258) throw new IOException("Invalid Xbox executable.");
    }
    private void extract(File stage) throws IOException {
        if (stage.getParentFile().getUsableSpace() < totalBytes+64L*1024*1024)
            throw new IOException("Not enough free space to extract the ISO.");
        byte[] bytes=new byte[1024*1024]; ByteBuffer buffer=ByteBuffer.wrap(bytes);
        long done=0, lastReport=0;
        String prefix=stage.getCanonicalPath()+File.separator;
        for (Entry e:entries) {
            checkCanceled(); File out=new File(stage,e.path);
            if (!out.getCanonicalPath().startsWith(prefix)) throw new IOException("Unsafe ISO filename.");
            File dir=e.directory?out:out.getParentFile();
            if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("Could not create game folder.");
            if (e.directory) continue;
            try (FileOutputStream stream=new FileOutputStream(out)) {
                long copied=0;
                while (copied < e.size) {
                    checkCanceled(); buffer.clear(); buffer.limit((int)Math.min(bytes.length,e.size-copied));
                    int n=source.read(buffer,e.offset+copied);
                    if (n <= 0) throw new IOException("Could not read ISO data.");
                    stream.write(bytes,0,n); copied+=n; done+=n;
                    long now=System.nanoTime();
                    if (now-lastReport > 100000000L) { progress.update("Extracting",done,totalBytes,e.path); lastReport=now; }
                }
            }
        }
        progress.update("Checking game",done,totalBytes,"default.xbe");
    }
    private String imageDigest(Entry entry) throws IOException {
        try {
            MessageDigest md=MessageDigest.getInstance("SHA-256");
            for (long at=0; at<entry.size;) {
                int n=(int)Math.min(65536,entry.size-at);
                md.update(read(entry.offset+at,n)); at+=n;
            }
            StringBuilder hex=new StringBuilder();
            for (byte b:md.digest()) hex.append(String.format(Locale.ROOT,"%02x",b&255));
            return hex.toString();
        } catch (java.security.NoSuchAlgorithmException e) { throw new IOException(e); }
    }

    private static String sha256(File file) throws IOException {
        try {
            MessageDigest md=MessageDigest.getInstance("SHA-256"); byte[] buffer=new byte[65536];
            try (FileInputStream in=new FileInputStream(file)) { int n; while ((n=in.read(buffer))!=-1) md.update(buffer,0,n); }
            StringBuilder hex=new StringBuilder(); for (byte b:md.digest()) hex.append(String.format(Locale.ROOT,"%02x",b&255));
            return hex.toString();
        } catch (java.security.NoSuchAlgorithmException e) { throw new IOException(e); }
    }
    private static void removeStage(File file) {
        File[] children=file.listFiles(); if (children!=null) for (File child:children) removeStage(child);
        if (file.exists() && !file.delete()) file.deleteOnExit();
    }
    /** Returns the previous game directory backup, or null for a fresh install. */
    public static File importIso(FileChannel channel, File root, AtomicBoolean canceled, Progress progress) throws IOException {
        return importIso(channel,root,canceled,progress,SUPPORTED_XBE_SHA256);
    }
    // Package-private digest argument lets fixtures use wholly synthetic game-free images.
    static File importIso(FileChannel channel, File root, AtomicBoolean canceled, Progress progress, String expectedDigest) throws IOException {
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("Could not create game folder.");
        new File(root,".nomedia").createNewFile(); // keep extracted game images out of gallery from the first write
        XisoExtractor reader=new XisoExtractor(channel,canceled,progress);
        progress.update("Reading ISO",0,0,""); reader.scan();
        Entry executable=null;
        for (Entry entry:reader.entries) if (entry.path.equals("default.xbe")) executable=entry;
        if (executable==null || executable.size > 16L*1024*1024
                || !reader.imageDigest(executable).equalsIgnoreCase(expectedDigest))
            throw new IOException("Unsupported game version. Use your original USA Xbox Def Jam: Fight for NY disc dump.");
        File stage=new File(root,".iso-import-"+UUID.randomUUID());
        if (!stage.mkdir()) throw new IOException("Could not create game folder.");
        File game=new File(root,"game"), backup=null;
        try {
            reader.extract(stage);
            if (!sha256(new File(stage,"default.xbe")).equalsIgnoreCase(expectedDigest))
                throw new IOException("Unsupported game version. Use your original USA Xbox Def Jam: Fight for NY disc dump.");
            reader.checkCanceled();
            // No cancellation inside the short commit; the previous tree remains recoverable.
            if (game.exists()) {
                backup=new File(root,"game-before-import-"+System.currentTimeMillis()+"-"+UUID.randomUUID());
                if (!game.renameTo(backup)) throw new IOException("Could not back up the previous game folder.");
            }
            if (!stage.renameTo(game)) {
                if (backup!=null && !backup.renameTo(game)) throw new IOException("Import failed. Previous game remains in: "+backup);
                throw new IOException("Could not install extracted files.");
            }
            return backup;
        } finally { if (stage.exists()) removeStage(stage); }
    }
    private XisoExtractor() { throw new AssertionError(); }
}
