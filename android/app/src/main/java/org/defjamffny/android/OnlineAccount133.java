package org.defjamffny.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.AtomicFile;
import android.util.Base64;
import android.util.Log;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Button;
import android.widget.ScrollView;
import org.json.JSONObject;
import javax.net.ssl.HttpsURLConnection;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import java.io.*;
import java.net.URL;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.util.concurrent.atomic.AtomicBoolean;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.text.Normalizer;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

/** Android adapter for FFNY Online's browser/poll account protocol (MIT).
 * No Discord credentials/client secret in the app. The bearer stays in app-private
 * AES-GCM storage backed by Android Keystore, never settings.ini or shared storage.
 */
final class OnlineAccount133 {
    private static final String BASE="https://play.defjam.online", KEY="defjam.account133";
    private static final ScheduledExecutorService IO=Executors.newScheduledThreadPool(2);
    private static final Handler UI=new Handler(Looper.getMainLooper());
    private static final AtomicInteger GENERATION=new AtomicInteger();
    private static final AtomicBoolean POLLING=new AtomicBoolean();
    private static Context app;
    private static byte[] installDeviceId;
    private static volatile int state; // same AccountState order as PC
    private static volatile String token="",name="",username="",reason="",stats="";
    private static volatile boolean bound,gameForeground;
    private static java.lang.ref.WeakReference<AlertDialog> entryDialog=new java.lang.ref.WeakReference<>(null);
    private static java.lang.ref.WeakReference<Activity> gameActivity=new java.lang.ref.WeakReference<>(null);
    static void gameForeground(boolean on){gameForeground=on;}
    static void openVoiceSettings(){UI.post(()->{Activity a=gameActivity.get();if(a instanceof DefJamActivity&&!a.isDestroyed())((DefJamActivity)a).openVoiceSettings133();});}
    private static native void nativeStats(boolean busy,String text);
    private static final AtomicBoolean STATS_BUSY=new AtomicBoolean();
    static void fetchStats(int page){
        if(state!=3||!STATS_BUSY.compareAndSet(false,true))return;
        final String t=token;final int gen=GENERATION.get();nativeStats(true,"");
        IO.execute(()->{String result="";try{
            Reply reply=request("GET","/api/me/stats?format=text&page="+Math.max(0,Math.min(1000,page)),t,null);
            if(reply.code==200)result=reply.body;else if(reply.code==401)expired(t);
        }catch(Exception ignored){}finally{nativeStats(false,gen==GENERATION.get()?result:"");STATS_BUSY.set(false);}});
    }
    private static native void nativeBind();
    private static native void nativeState(int state,String name,String text);
    private static String tr(String en,String pt,String es){return UiText.isPortuguese()?pt:UiText.isSpanish()?es:en;}
    static synchronized void init(Context context,boolean nativeAvailable){
        app=context.getApplicationContext();
        if(nativeAvailable&&context instanceof Activity)gameActivity=new java.lang.ref.WeakReference<>((Activity)context);
        if(nativeAvailable&&!bound){nativeBind();bound=true;}
    }
    private static void publish(){if(bound)nativeState(state,gameName(),reason);}
    static String gameName(){return state==3?gameName(name,username):"";}
    static String gameName(String display,String user){
        String s=Normalizer.normalize(display,Normalizer.Form.NFD).replaceAll("\\p{M}","").replaceAll("[^A-Za-z0-9 _.-]","").trim();
        if(s.isEmpty())s=user.replaceAll("[^A-Za-z0-9 _.-]","").trim();
        return s.substring(0,Math.min(24,s.length()));
    }
    private static boolean hex(String s,int n){return s!=null&&s.matches("[0-9a-fA-F]{"+n+"}");}
    private static AtomicFile file(){return new AtomicFile(new File(app.getNoBackupFilesDir(),"online-account133.bin"));}
    private static SecretKey key()throws Exception{
        KeyStore ks=KeyStore.getInstance("AndroidKeyStore");ks.load(null);
        if(!ks.containsAlias(KEY)){
            KeyGenerator g=KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES,"AndroidKeyStore");
            g.init(new KeyGenParameterSpec.Builder(KEY,KeyProperties.PURPOSE_ENCRYPT|KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).setKeySize(256).build());g.generateKey();
        }
        return (SecretKey)ks.getKey(KEY,null);
    }
    private static AtomicFile pending(){return new AtomicFile(new File(app.getNoBackupFilesDir(),"online-login133.bin"));}
    private static synchronized void saveValue(AtomicFile f,String t)throws Exception{
        Cipher c=Cipher.getInstance("AES/GCM/NoPadding");c.init(Cipher.ENCRYPT_MODE,key());
        JSONObject j=new JSONObject().put("iv",Base64.encodeToString(c.getIV(),Base64.NO_WRAP))
            .put("data",Base64.encodeToString(c.doFinal(t.getBytes(StandardCharsets.US_ASCII)),Base64.NO_WRAP));
        FileOutputStream o=f.startWrite();
        try{o.write(j.toString().getBytes(StandardCharsets.UTF_8));f.finishWrite(o);}catch(Exception e){f.failWrite(o);throw e;}
    }
    private static synchronized String loadValue(AtomicFile f)throws Exception{
        if(!f.getBaseFile().exists())return "";
        byte[] bytes=f.readFully();if(bytes.length>8192)throw new IOException("account size");
        JSONObject j=new JSONObject(new String(bytes,StandardCharsets.UTF_8));Cipher c=Cipher.getInstance("AES/GCM/NoPadding");
        c.init(Cipher.DECRYPT_MODE,key(),new GCMParameterSpec(128,Base64.decode(j.getString("iv"),Base64.NO_WRAP)));
        String t=new String(c.doFinal(Base64.decode(j.getString("data"),Base64.NO_WRAP)),StandardCharsets.US_ASCII);
        return t;
    }
    private static void save(String t)throws Exception{saveValue(file(),t);}
    private static String load()throws Exception{String t=loadValue(file());if(!t.isEmpty()&&!hex(t,64))throw new IOException("account token");return t;}
    private static boolean samePending(String secret)throws Exception{
        String data=loadValue(pending());return !data.isEmpty()&&new JSONObject(data).optString("secret").equals(secret);
    }
    private static void clearPending(String secret){try{if(samePending(secret))pending().delete();}catch(Exception ignored){}}
    private static boolean resumePending(int gen)throws Exception{
        String data=loadValue(pending());if(data.isEmpty())return false;
        JSONObject j=new JSONObject(data);String secret=j.optString("secret");long left=j.getLong("expires")-System.currentTimeMillis();
        if(!hex(secret,64)||left<=0||left>600000){pending().delete();return false;}
        if(gen!=GENERATION.get())return true;state=2;
        reason=tr("Finish sign-in in the browser, then return.","Conclua o login no navegador e volte.","Completa el inicio en el navegador y vuelve.");publish();
        long deadline=SystemClock.elapsedRealtime()+left;IO.schedule(()->poll(gen,secret,deadline,0),1,TimeUnit.SECONDS);return true;
    }
    private static final class Reply{final int code;final String body;Reply(int c,String b){code=c;body=b;}}
    private static Reply request(String method,String path,String bearer,String body)throws Exception{
        if(Looper.myLooper()==Looper.getMainLooper())throw new IllegalStateException("Network on UI thread");
        if(!path.startsWith("/")||path.contains("\r")||path.contains("\n"))throw new IOException("path");
        HttpsURLConnection c=(HttpsURLConnection)new URL(BASE+path).openConnection();
        c.setInstanceFollowRedirects(false);c.setConnectTimeout(7000);c.setReadTimeout(10000);c.setRequestMethod(method);
        long build = app.getPackageManager().getPackageInfo(app.getPackageName(), 0).getLongVersionCode();
        c.setRequestProperty("Accept", "application/json, text/plain");
        c.setRequestProperty("User-Agent", AccountProtocol133.userAgent(build));
        if(!bearer.isEmpty()){if(!hex(bearer,64))throw new IOException("token");c.setRequestProperty("Authorization","Bearer "+bearer);}
        try{
            if(body!=null){byte[] b=body.getBytes(StandardCharsets.UTF_8);c.setDoOutput(true);c.setRequestProperty("Content-Type","application/json");c.setFixedLengthStreamingMode(b.length);try(OutputStream o=c.getOutputStream()){o.write(b);}}
            int code=c.getResponseCode();
            if (code >= 400) Log.w("FFNYAccount", method + " " + path + " returned " + code + " (build " + build + ")");
            InputStream in=code>=400?c.getErrorStream():c.getInputStream();ByteArrayOutputStream out=new ByteArrayOutputStream();
            if(in!=null)try(InputStream i=in){byte[] b=new byte[2048];int n;while((n=i.read(b))!=-1){if(out.size()+n>65536)throw new IOException("response limit");out.write(b,0,n);}}
            return new Reply(code,out.toString("UTF-8"));
        }finally{c.disconnect();}
    }
    private static String accountFailure(Reply reply) {
        if (reply.code == 403) {
            String error = reply.body;
            try { error = new JSONObject(reply.body).optString("error", error); }
            catch (org.json.JSONException ignored) { }
            String lower = error.toLowerCase(java.util.Locale.ROOT);
            if (lower.contains("update"))
                return tr("Update the Android app to sign in.", "Atualize o app Android para entrar.", "Actualiza la app Android para iniciar sesión.");
            return tr("FFNY Online refused Android sign-in (HTTP 403).",
                "O FFNY Online recusou o login no Android (HTTP 403).",
                "FFNY Online rechazó el acceso en Android (HTTP 403).");
        }
        return tr("Account service unavailable.", "Serviço de conta indisponível.", "Servicio de cuenta no disponible.");
    }
    static void refresh(){
        if(app==null||state==2)return;final int gen=GENERATION.get();
        IO.execute(()->{try{
            if(resumePending(gen))return;
            String t=load();if(gen!=GENERATION.get())return;
            if(t.isEmpty()){token=name=username="";state=0;publish();return;}
            state=1;publish();Reply r=request("GET","/api/me",t,null);
            synchronized(OnlineAccount133.class){if(gen!=GENERATION.get())return;
                if(r.code==200){JSONObject a=new JSONObject(r.body);token=t;name=a.optString("name");username=a.optString("username");state=3;reason="";}
                else if(r.code==401){file().delete();token=name=username="";state=0;reason=tr("Sign in again.","Entre novamente.","Inicia sesión de nuevo.");}
                else{state=4;reason=accountFailure(r);}
                publish();}
        }catch(Exception e){if(gen==GENERATION.get()){state=4;reason=tr("Could not check the account.","Não foi possível verificar a conta.","No se pudo verificar la cuenta.");publish();}}});
    }
    static void signIn(Activity a){
        if(state==2)return;
        final int gen=GENERATION.incrementAndGet();final byte[] random=new byte[32];new SecureRandom().nextBytes(random);
        try{
            String secret=AccountProtocol133.hex(random),login=AccountProtocol133.loginHash(secret);
            state=2;reason=tr("Complete sign-in in the browser, then return.","Conclua o login no navegador e volte.","Completa el inicio en el navegador y vuelve.");publish();
            IO.execute(()->{
                try{
                    synchronized(OnlineAccount133.class){if(gen!=GENERATION.get())return;
                        saveValue(pending(),new JSONObject().put("secret",secret).put("expires",System.currentTimeMillis()+600000).toString());}
                    UI.post(()->{
                        if(gen!=GENERATION.get()||a.isDestroyed()){clearPending(secret);return;}
                        try{a.startActivity(new Intent(Intent.ACTION_VIEW,Uri.parse(BASE+"/auth/discord/start?login="+login)));
                            long deadline=SystemClock.elapsedRealtime()+600000;IO.schedule(()->poll(gen,secret,deadline,0),2,TimeUnit.SECONDS);
                        }catch(Exception e){clearPending(secret);state=4;reason=tr("Could not open sign-in.","Não foi possível abrir o login.","No se pudo abrir el inicio de sesión.");publish();}
                    });
                }catch(Exception e){if(gen==GENERATION.get()){state=4;reason=tr("Could not protect the login session.","Não foi possível proteger a sessão de login.","No se pudo proteger la sesión.");publish();}}
            });
        }catch(Exception e){state=4;reason=tr("Could not open sign-in.","Não foi possível abrir o login.","No se pudo abrir el inicio de sesión.");publish();}
        java.util.Arrays.fill(random,(byte)0);
    }

    private static void poll(int gen,String secret,long deadline,int misses){
        if(gen!=GENERATION.get())return;
        if(SystemClock.elapsedRealtime()>=deadline){clearPending(secret);state=4;reason=tr("Sign-in expired. Try again.","Login expirou. Tente novamente.","El inicio caducó. Inténtalo de nuevo.");publish();return;}
        if(!POLLING.compareAndSet(false,true)){final int count=misses;IO.schedule(()->poll(gen,secret,deadline,count),2,TimeUnit.SECONDS);return;}
        try(FileChannel channel=new FileOutputStream(new File(app.getNoBackupFilesDir(),"online-login133.lock"),true).getChannel()){
            FileLock lock=channel.tryLock();
            if(lock==null){final int count=misses;IO.schedule(()->poll(gen,secret,deadline,count),2,TimeUnit.SECONDS);return;}
            try(FileLock held=lock){
                if(!samePending(secret)){if(gen==GENERATION.get()){state=0;refresh();}return;}
                Reply r=request("POST","/auth/poll","",new JSONObject().put("secret",secret).toString());
                if(gen!=GENERATION.get()||!samePending(secret))return;
                if(r.code==200){JSONObject j=new JSONObject(r.body),account=j.getJSONObject("account");String t=j.getString("token");if(!hex(t,64))throw new IOException("token");
                    synchronized(OnlineAccount133.class){if(gen!=GENERATION.get()||!samePending(secret))return;save(t);clearPending(secret);token=t;name=account.optString("name");username=account.optString("username");state=3;reason="";publish();}return;}
                if(r.code==403){clearPending(secret);state=4;reason=accountFailure(r);publish();return;}
                if(r.code==410||(r.code==404&&SystemClock.elapsedRealtime()>deadline-570000)){clearPending(secret);state=4;reason=tr("Sign-in expired or cancelled.","Login expirou ou foi cancelado.","El inicio caducó o se canceló.");publish();return;}
                misses=r.code==202?0:misses+1;
            }
        }catch(Exception e){misses++;}
        finally{POLLING.set(false);}
        if(misses>=15){clearPending(secret);state=4;reason=tr("Account service unavailable.","Serviço de conta indisponível.","Servicio de cuenta no disponible.");publish();return;}
        final int next=misses;IO.schedule(()->poll(gen,secret,deadline,next),2,TimeUnit.SECONDS);
    }
    static void cancel(){GENERATION.incrementAndGet();pending().delete();if(state==2){state=0;reason="";publish();}}
    static void signOut(){
        final String old=token;GENERATION.incrementAndGet();synchronized(OnlineAccount133.class){file().delete();pending().delete();token=name=username=stats=reason="";state=0;publish();}
        if(!old.isEmpty())IO.execute(()->{try{request("POST","/auth/logout",old,"{}");}catch(Exception ignored){}});
    }
    private static synchronized void expired(String old){
        if(!token.equals(old))return;
        GENERATION.incrementAndGet();file().delete();token=name=username="";state=0;
        reason=tr("Sign in again.","Entre novamente.","Inicia sesión de nuevo.");publish();
    }
    static String rankedTicket() { return ""; }
    // Accessed by the native network worker and the match-report executor in :game.
    static synchronized byte[] deviceId() throws Exception {
        if (installDeviceId != null) return installDeviceId.clone();
        AtomicFile file = new AtomicFile(new File(app.getNoBackupFilesDir(), "ffny-install-id"));
        byte[] seed;
        try (FileInputStream input = file.openRead()) {
            seed = new byte[32];
            new DataInputStream(input).readFully(seed);
            if (input.read() != -1) throw new IOException("Invalid install ID seed");
        } catch (FileNotFoundException missing) {
            if (file.getBaseFile().exists()) throw missing;
            seed = new byte[32];
            new SecureRandom().nextBytes(seed);
            FileOutputStream output = file.startWrite();
            try {
                output.write(seed);
                file.finishWrite(output);
            } catch (IOException failure) {
                file.failWrite(output);
                throw failure;
            }
        }
        try {
            installDeviceId = AccountProtocol133.hashInstallId(seed);
            return installDeviceId.clone();
        } finally {
            java.util.Arrays.fill(seed, (byte)0);
        }
    }

    static void reportMatch(String json) {
        if (state != 3 || json == null || json.length() > 16384) return;
        final String currentToken = token;
        IO.execute(() -> {
            try {
                JSONObject report = new JSONObject(json);
                JSONObject fairPlay = report.optJSONObject("ac");
                if (fairPlay == null) fairPlay = new JSONObject();
                fairPlay.put("pc", AccountProtocol133.hex(deviceId()));
                report.put("ac", fairPlay);
                request("POST", "/api/matches", currentToken, report.toString());
            } catch (Exception ignored) { }
        });
    }
    static void enter(Activity a, Runnable accepted) { show(a, accepted, true); }
    static void show(Activity a) { show(a, null, false); }
    private static void show(Activity a,Runnable after,boolean entering){
        AlertDialog prior=entryDialog.get();if(prior!=null&&prior.isShowing())return;
        init(a,a instanceof DefJamActivity);refresh();
        LinearLayout body=new LinearLayout(a);body.setOrientation(LinearLayout.VERTICAL);int pad=PortUi.dp(a,18);body.setPadding(pad,pad,pad,pad);
        body.setBackground(PortUi.shape(a,0xff111010,0xff594327,18));
        TextView heading=PortUi.text(a,"ONLINE",26,0xffffe9b1,true);body.addView(heading);
        TextView status=PortUi.text(a,"",15,PortUi.TEXT,false);status.setPadding(0,pad,0,pad);body.addView(status);
        Button login=PortUi.button(a,tr("Sign in with Discord","Entrar com Discord","Entrar con Discord"),true);body.addView(login);
        Button cancel=PortUi.button(a,tr("Cancel sign-in","Cancelar login","Cancelar inicio"),false);body.addView(cancel);cancel.setOnClickListener(v->cancel());
        Button logout=PortUi.button(a,tr("Sign out","Sair da conta","Cerrar sesión"),false);body.addView(logout);logout.setOnClickListener(v->signOut());
        Button delete=PortUi.button(a,tr("Delete online account","Excluir conta online","Eliminar cuenta online"),false);body.addView(delete);
        delete.setOnClickListener(v->new AlertDialog.Builder(a).setTitle(tr("Delete online account?","Excluir conta online?","¿Eliminar cuenta online?"))
            .setMessage(tr("This removes your account and statistics from the PC account service. Game saves are retained.","Remove sua conta e estatísticas do serviço do PC. Os saves do jogo são mantidos.","Elimina la cuenta y estadísticas del servicio del PC. Las partidas guardadas se mantienen."))
            .setNegativeButton(UiText.t("Cancel"),null).setPositiveButton(tr("Delete","Excluir","Eliminar"),(d,w)->{
                final String t=token;if(t.isEmpty())return;
                IO.execute(()->{try{Reply reply=request("DELETE","/api/me",t,null);if(reply.code==204||reply.code==200||reply.code==401)expired(t);else{reason=tr("Could not delete the account.","Não foi possível excluir a conta.","No se pudo eliminar la cuenta.");publish();}}catch(Exception error){reason=tr("Account service unavailable.","Serviço de conta indisponível.","Servicio de cuenta no disponible.");publish();}});
            }).show());
        Button scores=PortUi.button(a,tr("My stats","Minhas estatísticas","Mis estadísticas"),false);body.addView(scores);
        TextView scoreText=PortUi.text(a,"",12,PortUi.MUTED,false);body.addView(scoreText);
        scores.setOnClickListener(v->IO.execute(()->{try{String t=token;Reply mine=t.isEmpty()?new Reply(200,""):request("GET","/api/me/stats?format=text&page=0",t,null);stats=mine.code==200?mine.body:accountFailure(mine);UI.post(()->scoreText.setText(stats));}catch(Exception e){UI.post(()->scoreText.setText(tr("Service unavailable","Serviço indisponível","Servicio no disponible")));}}));
        Button privacy=PortUi.button(a,tr("Privacy","Privacidade","Privacidad"),false);body.addView(privacy);privacy.setOnClickListener(v->{if(a instanceof DefJamActivity&&DefJamActivity.nativeOnlineSession()){
            android.widget.Toast.makeText(a,tr("Leave online before opening the browser.","Saia do online antes de abrir o navegador.","Sal del online antes de abrir el navegador."),android.widget.Toast.LENGTH_LONG).show();return;}
            a.startActivity(new Intent(Intent.ACTION_VIEW,Uri.parse(BASE+"/privacy")));});
        ScrollView scroll=new ScrollView(a);scroll.addView(body);
        for(int i=0;i<body.getChildCount();i++){
            android.view.View child=body.getChildAt(i);
            if(child instanceof Button){LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,PortUi.dp(a,48));lp.topMargin=PortUi.dp(a,8);child.setLayoutParams(lp);}
        }
        AlertDialog d=PortUi.dialog(a).setView(scroll).setPositiveButton(UiText.t("Close"),null).create();
        login.setOnClickListener(v->{if(a instanceof DefJamActivity&&DefJamActivity.nativeOnlineSession()){android.widget.Toast.makeText(a,tr("Leave the room before opening the browser.","Saia da sala antes de abrir o navegador.","Sal de la sala antes de abrir el navegador."),android.widget.Toast.LENGTH_LONG).show();return;}signIn(a);});
        Runnable tick=new Runnable(){public void run(){if(!d.isShowing()||a.isDestroyed())return;status.setText(state==3?name+" (@"+username+")":reason.isEmpty()?tr("Not signed in","Não conectado","Sin sesión"):reason);login.setVisibility(state==3||state==2?android.view.View.GONE:android.view.View.VISIBLE);
            cancel.setVisibility(state==2?android.view.View.VISIBLE:android.view.View.GONE);
            logout.setVisibility(state==3?android.view.View.VISIBLE:android.view.View.GONE);
            delete.setVisibility(!entering&&state==3?android.view.View.VISIBLE:android.view.View.GONE);
            scores.setVisibility(!entering&&state==3?android.view.View.VISIBLE:android.view.View.GONE);
            privacy.setVisibility(entering?android.view.View.GONE:android.view.View.VISIBLE);
            if(entering&&state==3&&gameForeground){d.dismiss();return;}UI.postDelayed(this,300);}};
        d.setOnDismissListener(v->{UI.removeCallbacks(tick);entryDialog.clear();
            if(!a.isDestroyed()&&!a.isFinishing()){
                if(a instanceof DefJamActivity)((DefJamActivity)a).restoreAfterOnline();
                if(after!=null&&state==3&&gameForeground)after.run();
            }});
        d.setOnShowListener(v->{if(d.getWindow()!=null)d.getWindow().setBackgroundDrawable(PortUi.shape(a,0xff111010,0xff594327,18));d.getButton(AlertDialog.BUTTON_POSITIVE).setTextColor(PortUi.GOLD);});
        entryDialog=new java.lang.ref.WeakReference<>(d);d.show();UI.post(tick);
    }
}
