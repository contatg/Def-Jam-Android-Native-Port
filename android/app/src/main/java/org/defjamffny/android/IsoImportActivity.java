package org.defjamffny.android;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.net.Uri;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import java.io.File;
import java.io.IOException;
import java.lang.ref.WeakReference;
import java.util.concurrent.atomic.AtomicBoolean;

/** Local SAF picker and one import worker. Activity recreation never starts a second extraction. */
public final class IsoImportActivity extends Activity {
    private static final int PICK_ISO=0x4953;
    private static ImportTask active;
    private TextView status, detail, percent, progressText, currentFile;
    private ProgressBar progress;
    private Button choose, cancel;
    private static final Handler MAIN=new Handler(Looper.getMainLooper());

    private static final class ImportTask implements Runnable {
        final android.content.Context app;
        final Uri uri;
        final AtomicBoolean canceled=new AtomicBoolean();
        volatile boolean running=true;
        volatile String phase="Reading ISO", file="", result;
        volatile long done, total;
        WeakReference<IsoImportActivity> observer=new WeakReference<>(null);
        ImportTask(android.content.Context context, Uri source) { app=context.getApplicationContext(); uri=source; }
        void refresh() { MAIN.post(() -> { IsoImportActivity a=observer.get(); if(a!=null&&!a.isFinishing()) a.render(this); }); }
        @Override public void run() {
            try {
                File root=new File(Environment.getExternalStorageDirectory(),"DefJamFFNY");
                ParcelFileDescriptor fd=app.getContentResolver().openFileDescriptor(uri,"r");
                if(fd==null) throw new IOException("Could not open ISO.");
                File backup;
                try (ParcelFileDescriptor.AutoCloseInputStream in=new ParcelFileDescriptor.AutoCloseInputStream(fd)) {
                    // Random reads avoid copying a multi-GB image into internal app storage.
                    // Cloud/pipe providers fail with a useful local-file message.
                    try { in.getChannel().position(0); in.getChannel().size(); }
                    catch(IOException e) { throw new IOException("Select an ISO stored locally on this phone.",e); }
                    backup=XisoExtractor.importIso(in.getChannel(),root,canceled,(p,d,t,f)->{
                        phase=p; done=d; total=t; file=f; refresh();
                    });
                }
                phase="Import complete.";
                result=new File(root,"game").getAbsolutePath();
                if(backup!=null) result+="\n"+UiText.t("Previous game backup: ")+backup.getAbsolutePath();
            } catch(Exception error) {
                phase=canceled.get()?"Import canceled.":"Import failed.";
                result=UiText.t(error.getMessage()==null?"Could not read ISO data.":error.getMessage());
            } finally { running=false; refresh(); }
        }
    }
    private int dp(float n) { return PortUi.dp(this,n); }
    private android.widget.LinearLayout column() {
        android.widget.LinearLayout l=new android.widget.LinearLayout(this);
        l.setOrientation(android.widget.LinearLayout.VERTICAL);return l;
    }
    private void gap(android.widget.LinearLayout parent,android.view.View view,int top) {
        android.widget.LinearLayout.LayoutParams p=new android.widget.LinearLayout.LayoutParams(-1,-2);
        p.topMargin=dp(top);parent.addView(view,p);
    }
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        UiText.setLanguage(getSharedPreferences("launcher",MODE_PRIVATE).getString("ui_language","en"));
        buildUi();
    }
    private void buildUi() {
        android.widget.FrameLayout root=new android.widget.FrameLayout(this);
        root.setBackground(new android.graphics.drawable.GradientDrawable(
                android.graphics.drawable.GradientDrawable.Orientation.TL_BR,
                new int[]{0xff171d28,PortUi.BG,0xff080b10}));
        android.widget.ScrollView scroll=new android.widget.ScrollView(this);
        scroll.setFillViewport(true);scroll.setClipToPadding(false);scroll.setPadding(dp(24),dp(20),dp(24),dp(20));
        android.widget.LinearLayout body=column();
        body.addView(PortUi.text(this,"GAME FILES",10,PortUi.GOLD,true));
        gap(body,PortUi.text(this,"Import Xbox ISO",26,PortUi.TEXT,true),7);
        gap(body,PortUi.text(this,"Your original game, ready on your phone.",13,PortUi.MUTED,false),5);
        android.widget.LinearLayout card=column();card.setPadding(dp(20),dp(18),dp(20),dp(18));
        card.setBackground(PortUi.shape(this,PortUi.CARD,PortUi.LINE,18));
        android.widget.LinearLayout headline=new android.widget.LinearLayout(this);
        headline.setGravity(android.view.Gravity.CENTER_VERTICAL);
        status=PortUi.text(this,"Choose an ISO to begin.",17,PortUi.TEXT,true);
        headline.addView(status,new android.widget.LinearLayout.LayoutParams(0,-2,1));
        percent=PortUi.text(this,"—",16,PortUi.GOLD,true);
        percent.setPadding(dp(12),dp(5),dp(12),dp(5));
        percent.setBackground(PortUi.shape(this,0xff26303d,0,9));
        headline.addView(percent,new android.widget.LinearLayout.LayoutParams(-2,-2));card.addView(headline);
        progress=new ProgressBar(this,null,android.R.attr.progressBarStyleHorizontal);progress.setMax(1000);
        android.graphics.drawable.LayerDrawable layers=new android.graphics.drawable.LayerDrawable(new android.graphics.drawable.Drawable[]{
                PortUi.shape(this,0xff313c4d,0,5),
                new android.graphics.drawable.ClipDrawable(PortUi.shape(this,PortUi.GOLD,0,5),
                        android.view.Gravity.LEFT,android.graphics.drawable.ClipDrawable.HORIZONTAL)});
        layers.setId(0,android.R.id.background);layers.setId(1,android.R.id.progress);
        progress.setProgressDrawable(layers);progress.setIndeterminateTintList(android.content.res.ColorStateList.valueOf(PortUi.GOLD));
        android.widget.LinearLayout.LayoutParams pp=new android.widget.LinearLayout.LayoutParams(-1,dp(8));pp.topMargin=dp(16);card.addView(progress,pp);
        progressText=PortUi.text(this,"Local ISO / XISO",12,PortUi.MUTED,false);gap(card,progressText,10);
        currentFile=PortUi.text(this,"USA • Original Xbox",12,PortUi.MUTED,false);
        currentFile.setSingleLine(true);currentFile.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);gap(card,currentFile,5);
        detail=PortUi.text(this,"Select an uncompressed ISO/XISO from your own USA Xbox disc.",12,PortUi.MUTED,false);
        detail.setTextIsSelectable(true);gap(card,detail,10);
        android.widget.LinearLayout actions=new android.widget.LinearLayout(this);
        choose=PortUi.button(this,"Choose ISO",true);cancel=PortUi.button(this,"Cancel",false);
        Button close=PortUi.button(this,"Back",false);
        android.view.View[] buttons={choose,cancel,close};
        for(int i=0;i<buttons.length;i++) {
            android.widget.LinearLayout.LayoutParams q=new android.widget.LinearLayout.LayoutParams(0,-2,i==0?1.3f:1f);
            if(i>0)q.leftMargin=dp(8);actions.addView(buttons[i],q);
        }
        gap(card,actions,16);gap(body,card,18);
        gap(body,PortUi.text(this,"Saves stay safe. Your previous game folder is kept as a backup.",12,PortUi.MUTED,false),12);
        scroll.addView(body,new android.widget.ScrollView.LayoutParams(-1,-2));
        android.widget.FrameLayout.LayoutParams sp=new android.widget.FrameLayout.LayoutParams(
                Math.min(dp(860),getResources().getDisplayMetrics().widthPixels),-1,android.view.Gravity.CENTER_HORIZONTAL);
        root.addView(scroll,sp);setContentView(root);
        choose.setOnClickListener(v->{
            Intent i=new Intent(Intent.ACTION_OPEN_DOCUMENT);i.addCategory(Intent.CATEGORY_OPENABLE);i.setType("*/*");
            try { startActivityForResult(i,PICK_ISO); }
            catch(android.content.ActivityNotFoundException error) {status.setText(UiText.t("No file picker is available."));}
        });
        cancel.setOnClickListener(v->{
            if(active!=null&&active.running){active.canceled.set(true);cancel.setEnabled(false);status.setText(UiText.t("Canceling…"));}
        });
        close.setOnClickListener(v->onBackPressed());
        if(active!=null){active.observer=new WeakReference<>(this);render(active);}else cancel.setEnabled(false);
    }
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if(focused) {
            if(android.os.Build.VERSION.SDK_INT>=30) {
                android.view.WindowInsetsController bars=getWindow().getInsetsController();
                if(bars!=null){bars.hide(android.view.WindowInsets.Type.systemBars());
                    bars.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);}
            } else getWindow().getDecorView().setSystemUiVisibility(android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    |android.view.View.SYSTEM_UI_FLAG_FULLSCREEN|android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION);
        }
    }
    @Override public void onConfigurationChanged(android.content.res.Configuration config) {
        super.onConfigurationChanged(config);buildUi();
    }
    private void render(ImportTask task) {
        choose.setEnabled(!task.running);cancel.setEnabled(task.running&&!task.canceled.get());
        status.setText(UiText.t(task.running&&task.canceled.get()?"Canceling…":task.phase));
        status.setTextColor(task.phase.equals("Import failed.")?PortUi.RED:
                task.phase.equals("Import complete.")?PortUi.GREEN:PortUi.TEXT);
        progress.setIndeterminate(task.running&&task.total==0);
        int value=task.total>0?(int)Math.min(1000,task.done*1000/task.total):0;
        progress.setProgress(value);percent.setText(task.total>0?(value/10)+"%":task.running?"…":"—");
        progressText.setText(task.total>0?String.format(java.util.Locale.ROOT,"%d / %d MiB",task.done/1048576,task.total/1048576):UiText.t("Local ISO / XISO"));
        currentFile.setText(task.file.isEmpty()?UiText.t("USA • Original Xbox"):task.file);
        if(task.running){
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            detail.setText(UiText.t("Keep this screen open until extraction finishes."));
        }else{
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            detail.setText(task.result);
        }
    }
    @Override protected void onActivityResult(int request,int result,Intent data) {
        super.onActivityResult(request,result,data);
        if(request!=PICK_ISO||result!=RESULT_OK||data==null||data.getData()==null||active!=null&&active.running) return;
        active=new ImportTask(this,data.getData()); active.observer=new WeakReference<>(this); render(active);
        new Thread(active,"XboxIsoImport").start();
    }
    @Override public void onBackPressed() {
        if(active!=null&&active.running) {
            PortUi.show(PortUi.dialog(this).setMessage(UiText.t("Cancel ISO import?"))
                    .setNegativeButton(UiText.t("Continue import"),null)
                    .setPositiveButton(UiText.t("Cancel import"),(d,w)->{
                        active.canceled.set(true); cancel.setEnabled(false);
                    }));
            return;
        }
        finish();
    }
    @Override protected void onDestroy() {
        if(active!=null&&active.observer.get()==this) active.observer.clear();
        super.onDestroy();
    }
}
