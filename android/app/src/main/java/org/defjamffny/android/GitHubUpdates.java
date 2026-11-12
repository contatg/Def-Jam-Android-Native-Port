package org.defjamffny.android;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

/** Public release lookup. Launcher checks once per process; manual checks remain available. No credentials or APK execution. */
final class GitHubUpdates {
    static final class LookupException extends IOException {
        final String label;
        LookupException(String label) { super(label); this.label=label; }
    }
    static ReleaseUpdate check() throws Exception {
        ReleaseUpdate update=ReleaseUpdate.fromFeed(read(ReleaseUpdate.API,2*1024*1024,false));
        if(update!=null&&!update.metadata.isEmpty()) {
            try { update.applyMetadata(read(update.metadata,16384,true)); }
            catch(Exception ignored) { /* Version tags remain usable when optional metadata is unavailable. */ }
        }
        return update;
    }
    private static String read(String source,int limit,boolean asset) throws IOException {
        URL url=new URL(source);
        for(int redirects=0;redirects<5;redirects++) {
            if(Thread.currentThread().isInterrupted())throw new IOException("canceled");
            HttpURLConnection c=(HttpURLConnection)url.openConnection();
            c.setInstanceFollowRedirects(false);c.setConnectTimeout(8000);c.setReadTimeout(8000);
            c.setRequestProperty("User-Agent","DefJam-Android-Update-Check");
            c.setRequestProperty("Accept",asset?"application/json":"application/vnd.github+json");
            try {
                int code=c.getResponseCode();
                if(code==301||code==302||code==303||code==307||code==308) {
                    URL next=new URL(url,c.getHeaderField("Location"));String host=next.getHost();
                    if(!asset||!"https".equals(next.getProtocol())||next.getUserInfo()!=null||next.getPort()!=-1
                            || !(host.equals("github.com")||host.equals("release-assets.githubusercontent.com")
                            ||host.equals("objects.githubusercontent.com")))throw new IOException("redirect");
                    url=next;continue;
                }
                if(code==403||code==429)throw new LookupException("GitHub is busy. Try again later.");
                if(code==404)throw new LookupException("Releases are unavailable or private.");
                if(code!=200)throw new IOException("HTTP "+code);
                if(c.getContentLengthLong()>limit)throw new IOException("response too large");
                try(InputStream in=c.getInputStream();ByteArrayOutputStream out=new ByteArrayOutputStream()) {
                    byte[] b=new byte[4096];int n;
                    while((n=in.read(b))!=-1) {
                        if(out.size()+n>limit||Thread.currentThread().isInterrupted())throw new IOException("response too large/canceled");
                        out.write(b,0,n);
                    }
                    return new String(out.toByteArray(),StandardCharsets.UTF_8);
                }
            } finally { c.disconnect(); }
        }
        throw new IOException("redirect limit");
    }
    private static final java.util.concurrent.atomic.AtomicBoolean AUTO_CHECKED=new java.util.concurrent.atomic.AtomicBoolean();
    private static final android.os.Handler UI=new android.os.Handler(android.os.Looper.getMainLooper());
    private static ReleaseUpdate pending;
    private static String tr(String en,String pt,String es){return UiText.isPortuguese()?pt:UiText.isSpanish()?es:en;}
    static void onLauncherResume(android.app.Activity a){
        UI.postDelayed(()->showPending(a),500);
        if(!AUTO_CHECKED.compareAndSet(false,true))return;
        Thread worker=new Thread(()->{
            try{
                android.content.pm.PackageInfo info=a.getPackageManager().getPackageInfo(a.getPackageName(),0);
                ReleaseUpdate update=check();
                if(update==null||!"Update available".equals(update.status(info.versionName)))return;
                UI.post(()->{pending=update;showPending(a);});
            }catch(Exception ignored){ /* Offline/rate-limited checks never block launcher or gameplay. */ }
        },"dj-update134");worker.setDaemon(true);worker.start();
    }
    private static void showPending(android.app.Activity a){
        if(pending==null||a.isFinishing()||a.isDestroyed()||!a.hasWindowFocus())return;
        ReleaseUpdate update=pending;pending=null;
        android.widget.LinearLayout box=new android.widget.LinearLayout(a);box.setOrientation(1);
        int pad=PortUi.dp(a,20);box.setPadding(pad,pad,pad,pad);box.setBackground(PortUi.shape(a,0xff111010,0xff594327,18));
        box.addView(PortUi.text(a,tr("UPDATE AVAILABLE","ATUALIZAÇÃO DISPONÍVEL","ACTUALIZACIÓN DISPONIBLE"),22,PortUi.GOLD,true));
        android.widget.TextView title=PortUi.text(a,update.name,17,PortUi.TEXT,true);title.setPadding(0,pad,0,pad);box.addView(title);
        String notes=update.notes.isEmpty()?tr("A new version is available on GitHub.","Uma nova versão está disponível no GitHub.","Hay una nueva versión en GitHub."):update.notes;
        // Display notes as plain text; no remote HTML, WebView or script execution.
        box.addView(PortUi.text(a,notes.substring(0,Math.min(notes.length(),8000)),13,PortUi.MUTED,false));
        android.widget.ScrollView scroll=new android.widget.ScrollView(a);scroll.addView(box);
        PortUi.show(PortUi.dialog(a).setView(scroll)
            .setNegativeButton(tr("Later","Depois","Más tarde"),null)
            .setPositiveButton(tr("Download APK","Baixar APK","Descargar APK"),(d,w)->{
                try{a.startActivity(new android.content.Intent(android.content.Intent.ACTION_VIEW,android.net.Uri.parse(update.apk)));}
                catch(Exception ignored){android.widget.Toast.makeText(a,UiText.t("No browser is available."),android.widget.Toast.LENGTH_LONG).show();}
            }));
    }
    private GitHubUpdates() { }
}
