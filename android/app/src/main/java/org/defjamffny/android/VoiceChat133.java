package org.defjamffny.android;

import android.Manifest;
import android.app.AlertDialog;
import android.content.pm.PackageManager;
import android.media.*;
import android.os.Process;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.widget.*;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicInteger;

/** Host-only, opt-in voice. Receive/play and microphone processing are independent
 * of the guest, Vulkan and Xbox mixer. PC-compatible P_VOICE payloads via the relay.
 */
final class VoiceChat133 {
    private static native void nativeEnabled(boolean enabled);
    private static native long nativeRoom();
    private static native byte[] nativePop(long room);
    private static native void nativeSend(long room,byte[] packet);
    private final DefJamActivity activity;
    private final AtomicInteger generation=new AtomicInteger();
    private volatile boolean ptt,enabled;
    private volatile int volume=100;
    private volatile String status="";
    private Button mic,mute;
    private volatile boolean micMuted;
    private volatile boolean foreground;
    private int pointer=-1;
    private static String tr(String en,String pt,String es){return UiText.isPortuguese()?pt:UiText.isSpanish()?es:en;}
    VoiceChat133(DefJamActivity a){activity=a;enabled=a.getSharedPreferences("voice133",0).getBoolean("enabled",false);}
    void resume(){foreground=true;
        android.content.SharedPreferences p=activity.getSharedPreferences("voice133",0);
        if(!p.getBoolean("mic_prompt134",false)&&!DefJamActivity.nativeOnlineSession()){
            p.edit().putBoolean("mic_prompt134",true).apply();requestMicrophone();
        }
        if(enabled)start();}
    void pause(){foreground=false;ptt=false;pointer=-1;generation.incrementAndGet();nativeEnabled(false);if(mic!=null)mic.setVisibility(View.GONE);if(mute!=null)mute.setVisibility(View.GONE);}
    void destroy(){pause();if(mic!=null&&mic.getParent() instanceof android.view.ViewGroup)((android.view.ViewGroup)mic.getParent()).removeView(mic);mic=null;if(mute!=null&&mute.getParent() instanceof android.view.ViewGroup)((android.view.ViewGroup)mute.getParent()).removeView(mute);mute=null;}
    private void start(){
        int gen=generation.incrementAndGet();nativeEnabled(true);
        if(mic==null){
            mic=PortUi.button(activity,"MIC",false);
            FrameLayout.LayoutParams p=new FrameLayout.LayoutParams(PortUi.dp(activity,64),PortUi.dp(activity,48),Gravity.BOTTOM|Gravity.CENTER_HORIZONTAL);
            p.bottomMargin=PortUi.dp(activity,78);activity.addContentView(mic,p);
            mic.setOnTouchListener((v,e)->{
                int action=e.getActionMasked();
                if(action==MotionEvent.ACTION_DOWN){
                    if(activity.checkSelfPermission(Manifest.permission.RECORD_AUDIO)!=PackageManager.PERMISSION_GRANTED){
                        android.widget.Toast.makeText(activity,tr("Allow the microphone in Voice settings before joining a room.","Permita o microfone nas configurações de voz antes de entrar na sala.","Permite el micrófono en los ajustes de voz antes de entrar en una sala."),android.widget.Toast.LENGTH_LONG).show();return true;
                    }
                    if(micMuted)return true;
                    pointer=e.getPointerId(0);ptt=true;mic.setText("● MIC");return true;
                }
                if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL||
                    (action==MotionEvent.ACTION_POINTER_UP&&e.getPointerId(e.getActionIndex())==pointer)){
                    ptt=false;pointer=-1;mic.setText("MIC");return true;
                }
                return true;
            });
        }
        if(mute==null){
            mute=PortUi.button(activity,tr("Mute mic","Mutar mic","Silenciar mic"),false);
            FrameLayout.LayoutParams p=new FrameLayout.LayoutParams(PortUi.dp(activity,92),PortUi.dp(activity,48),Gravity.BOTTOM|Gravity.CENTER_HORIZONTAL);
            p.bottomMargin=PortUi.dp(activity,132);activity.addContentView(mute,p);
            mute.setOnClickListener(v->{micMuted=!micMuted;if(micMuted)ptt=false;
                mute.setText(micMuted?tr("Unmute mic","Ativar mic","Activar mic"):tr("Mute mic","Mutar mic","Silenciar mic"));if(mic!=null)mic.setText("MIC");});
        }
        mic.setVisibility(View.GONE);mute.setVisibility(View.GONE);
        Thread t=new Thread(()->run(gen),"dj-voice133");t.setDaemon(true);t.start();
    }
    void settings(){
        LinearLayout box=new LinearLayout(activity);box.setOrientation(LinearLayout.VERTICAL);int pad=PortUi.dp(activity,18);box.setPadding(pad,pad,pad,pad);box.setBackground(PortUi.shape(activity,0xff111010,0xff594327,18));
        box.addView(PortUi.text(activity,tr("VOICE CHAT","CHAT DE VOZ","CHAT DE VOZ"),24,PortUi.GOLD,true));
        TextView hint=PortUi.text(activity,tr("Room voice, compatible with PC. Hold MIC to talk. Allow microphone access here before joining a room. Headphones recommended.",
            "Voz da sala compatível com PC. Segure MIC para falar. Permita o microfone aqui antes de entrar na sala. Use fones.",
            "Voz de sala compatible con PC. Mantén MIC para hablar. Permite el micrófono aquí antes de entrar en la sala. Usa auriculares."),14,PortUi.TEXT,false);box.addView(hint);
        Button toggle=PortUi.button(activity,voiceLabel135(),true);box.addView(toggle);
        Button permission=PortUi.button(activity,tr("Allow microphone","Permitir microfone","Permitir micrófono"),false);box.addView(permission);
        permission.setVisibility(activity.checkSelfPermission(Manifest.permission.RECORD_AUDIO)==PackageManager.PERMISSION_GRANTED?View.GONE:View.VISIBLE);
        permission.setOnClickListener(v->requestMicrophone());
        TextView label=PortUi.text(activity,tr("Playback volume","Volume de reprodução","Volumen de reproducción"),13,PortUi.TEXT,false);box.addView(label);
        SeekBar slider=new SeekBar(activity);slider.setMax(200);slider.setProgress(volume);
        slider.setProgressTintList(android.content.res.ColorStateList.valueOf(PortUi.GOLD));
        slider.setThumbTintList(android.content.res.ColorStateList.valueOf(PortUi.GOLD));
        slider.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(0xff32343b));box.addView(slider);slider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){public void onProgressChanged(SeekBar s,int n,boolean user){volume=n;}public void onStartTrackingTouch(SeekBar s){}public void onStopTrackingTouch(SeekBar s){}});
        TextView info=PortUi.text(activity,status,12,PortUi.MUTED,false);box.addView(info);
        toggle.setOnClickListener(v->{boolean on=!enabled;enabled=on;toggle.setText(voiceLabel135());activity.getSharedPreferences("voice133",0).edit().putBoolean("enabled",on).apply();if(on&&foreground){if(!DefJamActivity.nativeOnlineSession())requestMicrophone();start();}else pauseKeepingForeground();});
        Button close=PortUi.button(activity,UiText.t("Close"),false);box.addView(close);
        for(int i=0;i<box.getChildCount();i++){
            android.view.View child=box.getChildAt(i);LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,child instanceof Button?PortUi.dp(activity,48):-2);
            lp.topMargin=PortUi.dp(activity,i==0?0:12);child.setLayoutParams(lp);
        }
        ScrollView scroll=new ScrollView(activity);scroll.addView(box);
        AlertDialog dialog=PortUi.dialog(activity).setView(scroll).create();
        close.setOnClickListener(v->dialog.dismiss());dialog.show();
        if(dialog.getWindow()!=null)dialog.getWindow().setBackgroundDrawable(PortUi.shape(activity,0xff111010,0xff594327,18));
    }
    private String voiceLabel135(){return enabled?tr("Voice chat: ON","Chat de voz: LIGADO","Chat de voz: ACTIVADO"):tr("Voice chat: OFF","Chat de voz: DESLIGADO","Chat de voz: DESACTIVADO");}
    private void requestMicrophone(){
        if(activity.checkSelfPermission(Manifest.permission.RECORD_AUDIO)==PackageManager.PERMISSION_GRANTED)return;
        if(DefJamActivity.nativeOnlineSession()){
            android.widget.Toast.makeText(activity,tr("Leave online before granting microphone permission.","Saia do online antes de permitir o microfone.","Sal del online antes de permitir el micrófono."),android.widget.Toast.LENGTH_LONG).show();return;
        }
        activity.requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO},0x133);
    }
    private void pauseKeepingForeground(){boolean was=foreground;pause();foreground=was;}
    private void run(int gen){
        try{Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);}catch(SecurityException ignored){}
        AudioTrack track=null;AudioRecord record=null;long room=0,written=0;
        int[][] ring=new int[10][3840];int[] rd=new int[10],wr=new int[10],seqIn=new int[10];boolean[] seen=new boolean[10];
        short[] capture=new short[640],decoded=new short[640],mix=new short[320];byte[] outgoing=new byte[326];int captured=0,seq=0,mixOffset=0,mixPending=0;boolean shown=false;VoiceCodec133.State codec=new VoiceCodec133.State();
        try{
            while(gen==generation.get()&&enabled&&foreground){
                long next=nativeRoom();
                if(next!=room){
                    if(record!=null){record.stop();record.release();record=null;}
                    if(track!=null){track.pause();track.flush();track.release();track=null;}
                    room=next;written=0;captured=0;mixPending=mixOffset=0;ptt=false;codec=new VoiceCodec133.State();Arrays.fill(rd,0);Arrays.fill(wr,0);Arrays.fill(seen,false);
                }
                final boolean visible=room!=0;
                if(visible!=shown){shown=visible;activity.runOnUiThread(()->{if(gen==generation.get()&&mic!=null){mic.setVisibility(visible?View.VISIBLE:View.GONE);if(mute!=null)mute.setVisibility(visible?View.VISIBLE:View.GONE);mic.setText("MIC");}});}
                if(room==0){Thread.sleep(40);continue;}
                if(track==null){
                    int n=AudioTrack.getMinBufferSize(16000,AudioFormat.CHANNEL_OUT_MONO,AudioFormat.ENCODING_PCM_16BIT);
                    if(n<=0)throw new IllegalStateException("voice playback unavailable");
                    track=new AudioTrack.Builder().setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
                        .setAudioFormat(new AudioFormat.Builder().setSampleRate(16000).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                        .setTransferMode(AudioTrack.MODE_STREAM).setBufferSizeInBytes(Math.max(n,2560)).build();
                    if(track.getState()!=AudioTrack.STATE_INITIALIZED)throw new IllegalStateException("voice playback initialization");track.play();status=tr("Voice connected. Hold MIC to talk.","Voz conectada. Segure MIC para falar.","Voz conectada. Mantén MIC para hablar.");
                }
                for(int packets=0;packets<16;packets++){
                    byte[] raw=nativePop(room);if(raw==null)break;if(raw.length!=327)continue;int slot=raw[0]&255;if(slot>=10)continue;
                    int incoming=(raw[2]&255)|((raw[3]&255)<<8);
                    if(seen[slot]&&!VoiceCodec133.newer(incoming,seqIn[slot]))continue;seen[slot]=true;seqIn[slot]=incoming;
                    VoiceCodec133.decodeInto(raw,1,decoded);
                    if(wr[slot]-rd[slot]+640>3840)rd[slot]=Math.max(rd[slot],wr[slot]-1280);
                    for(short x:decoded)ring[slot][wr[slot]++%3840]=x;
                }
                long head=Integer.toUnsignedLong(track.getPlaybackHeadPosition());
                if(mixPending==0&&written-head<=960){
                    for(int i=0;i<320;i++){int sample=0;for(int slot=0;slot<10;slot++)if(rd[slot]!=wr[slot])sample+=ring[slot][rd[slot]++%3840];sample=sample*volume/100;mix[i]=(short)Math.max(-32768,Math.min(32767,sample));}
                    mixOffset=0;mixPending=320;
                }
                if(mixPending>0){int count=track.write(mix,mixOffset,mixPending,AudioTrack.WRITE_NON_BLOCKING);if(count<0)throw new IllegalStateException("voice write");mixOffset+=count;mixPending-=count;written+=count;}
                if(ptt&&!micMuted&&activity.checkSelfPermission(Manifest.permission.RECORD_AUDIO)==PackageManager.PERMISSION_GRANTED){
                    if(record==null){int n=AudioRecord.getMinBufferSize(16000,AudioFormat.CHANNEL_IN_MONO,AudioFormat.ENCODING_PCM_16BIT);if(n<=0)throw new IllegalStateException("voice microphone unavailable");
                        record=new AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION,16000,AudioFormat.CHANNEL_IN_MONO,AudioFormat.ENCODING_PCM_16BIT,Math.max(n,5120));
                        if(record.getState()!=AudioRecord.STATE_INITIALIZED)throw new IllegalStateException("voice microphone initialization");record.startRecording();captured=0;}
                    int n=record.read(capture,captured,640-captured,AudioRecord.READ_NON_BLOCKING);
                    if(n<0)throw new IllegalStateException("voice read");captured+=n;
                    if(captured==640){if(gen==generation.get()&&ptt&&!micMuted&&foreground){VoiceCodec133.encodeInto(capture,seq++&65535,codec,outgoing);nativeSend(room,outgoing);}captured=0;}
                }else if(record!=null){record.stop();record.release();record=null;captured=0;}
                for(int slot=0;slot<10;slot++)if(rd[slot]>1000000){int shift=(rd[slot]/3840)*3840;rd[slot]-=shift;wr[slot]-=shift;}
                Thread.sleep(10);
            }
        }catch(InterruptedException ignored){Thread.currentThread().interrupt();}
        catch(Exception e){status=tr("Voice unavailable; game audio is unchanged.","Voz indisponível; áudio do jogo mantido.","Voz no disponible; el audio del juego se mantiene.");android.util.Log.w("VOICE133","Voice audio stopped (no microphone or packet contents logged)");}
        finally{
            if(record!=null){try{record.stop();}catch(Exception ignored){}record.release();}
            if(track!=null){try{track.stop();}catch(Exception ignored){}track.release();}
            if(gen==generation.get()){ptt=false;nativeEnabled(false);activity.runOnUiThread(()->{if(mic!=null)mic.setVisibility(View.GONE);if(mute!=null)mute.setVisibility(View.GONE);});}
        }
    }
}
