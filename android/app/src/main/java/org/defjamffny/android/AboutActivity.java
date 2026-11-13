package org.defjamffny.android;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Branded, scrollable project information and user-requested release checks. */
public final class AboutActivity extends Activity {
    private final ExecutorService worker=Executors.newSingleThreadExecutor();
    private LinearLayout content;
    private ScrollView scroll;
    private Button aboutTab,updatesTab;
    private String version="—",message="Check for a new release from contatg.";
    private long code;
    private boolean updates,document,checking,checked;
    private ReleaseUpdate release;
    private int request;
    private int dp(float n) { return PortUi.dp(this,n); }
    private LinearLayout column() { LinearLayout l=new LinearLayout(this);l.setOrientation(1);return l; }
    private TextView text(String s,float n,int color,boolean bold) { return PortUi.text(this,s,n,color,bold); }
    private void add(LinearLayout parent,View v,int top) {
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,-2);p.topMargin=dp(top);parent.addView(v,p);
    }
    private LinearLayout card() {
        return card(content);
    }
    private LinearLayout card(LinearLayout parent) {
        LinearLayout l=column();l.setPadding(dp(18),dp(16),dp(18),dp(16));
        l.setBackground(PortUi.shape(this,PortUi.CARD,PortUi.LINE,16));add(parent,l,12);return l;
    }
    private void action(LinearLayout parent,String label,boolean primary,Runnable click) {
        Button b=PortUi.button(this,label,primary);add(parent,b,12);b.setOnClickListener(v->click.run());
    }
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        UiText.setLanguage(getSharedPreferences("launcher",MODE_PRIVATE).getString("ui_language","en"));
        try { PackageInfo p=getPackageManager().getPackageInfo(getPackageName(),0);
            version=p.versionName;code=p.getLongVersionCode(); } catch(Exception ignored) { }
        updates=state!=null?state.getBoolean("updates"):getIntent().getBooleanExtra("updates",false);
        buildUi();render();if(updates)check();
    }
    private void buildUi() {
        LinearLayout root=column();root.setPadding(dp(24),dp(16),dp(24),dp(16));
        root.setBackground(new GradientDrawable(GradientDrawable.Orientation.TL_BR,
                new int[]{0xff1b2330,PortUi.BG,0xff090c11}));
        LinearLayout header=new LinearLayout(this);header.setGravity(Gravity.CENTER_VERTICAL);
        ImageView icon=new ImageView(this);icon.setImageDrawable(getApplicationInfo().loadIcon(getPackageManager()));
        icon.setContentDescription("Def Jam");header.addView(icon,new LinearLayout.LayoutParams(dp(52),dp(52)));
        LinearLayout titles=column();titles.setPadding(dp(14),0,dp(12),0);
        titles.addView(text("Def Jam",25,PortUi.TEXT,true));add(titles,text("Android Native Port  /  contatg",11,PortUi.GOLD,false),5);
        header.addView(titles,new LinearLayout.LayoutParams(0,-2,1));
        Button back=PortUi.button(this,"Back",false);back.setOnClickListener(v->goBack());header.addView(back);
        root.addView(header);
        LinearLayout tabs=new LinearLayout(this);tabs.setPadding(0,dp(14),0,dp(2));
        aboutTab=PortUi.button(this,"About",false);updatesTab=PortUi.button(this,"Updates",false);
        tabs.addView(aboutTab,new LinearLayout.LayoutParams(0,-2,1));
        LinearLayout.LayoutParams t=new LinearLayout.LayoutParams(0,-2,1);t.leftMargin=dp(10);tabs.addView(updatesTab,t);
        aboutTab.setOnClickListener(v->{updates=false;render();});
        updatesTab.setOnClickListener(v->{updates=true;render();if(!checked&&!checking)check();});
        root.addView(tabs);
        scroll=new ScrollView(this);scroll.setFillViewport(true);scroll.setClipToPadding(false);
        content=column();content.setPadding(0,0,0,dp(12));scroll.addView(content);
        root.addView(scroll,new LinearLayout.LayoutParams(-1,0,1));setContentView(root);
    }
    private void render() {
        document=false;content.removeAllViews();
        aboutTab.setTextColor(updates?PortUi.MUTED:PortUi.GOLD);
        updatesTab.setTextColor(updates?PortUi.GOLD:PortUi.MUTED);
        if(updates)renderUpdates();else renderAbout();
        scroll.scrollTo(0,0);
    }
    private void renderAbout() {
        LinearLayout left=content,right=content;
        if(getResources().getConfiguration().screenWidthDp>=680) {
            LinearLayout row=new LinearLayout(this);row.setBaselineAligned(false);
            left=column();right=column();
            row.addView(left,new LinearLayout.LayoutParams(0,-2,1));
            LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(0,-2,1);p.leftMargin=dp(16);row.addView(right,p);
            content.addView(row);
        }
        LinearLayout app=card(left);app.addView(text("PRE-RELEASE",10,PortUi.GOLD,true));
        add(app,text(AboutInfo.TITLE,20,PortUi.TEXT,true),8);
        add(app,text(UiText.t("Version")+" "+version,13,PortUi.MUTED,false),7);
        add(app,text("An experimental Android release. Bugs and instability may occur.",13,PortUi.MUTED,false),12);
        LinearLayout owner=card(left);owner.addView(text("Maintained by",10,PortUi.MUTED,false));
        add(owner,text(AboutInfo.MAINTAINER,19,PortUi.TEXT,true),6);
        add(owner,text("contatg / Def-Jam-Android-Native-Port",12,PortUi.MUTED,false),7);
        action(owner,"GitHub",true,()->open(AboutInfo.GITHUB));
        LinearLayout help=card(right);help.addView(text("Get started",16,PortUi.TEXT,true));
        add(help,text("Your own original Xbox disc/ISO is required. Original game data is not included.",13,PortUi.MUTED,false),8);
        action(help,"Setup guide",false,()->showDocument("Setup guide",AboutInfo.setup()));
        action(help,"Credits",false,()->showDocument("Credits",AboutInfo.credits(version)));
        action(help,"Third-party licenses",false,this::licenses);
    }
    private void renderUpdates() {
        LinearLayout status=card();status.addView(text("GITHUB RELEASES",10,PortUi.GOLD,true));
        String heading="Build information unavailable".equals(message)
                ? UiText.tr("Version unavailable","Versão indisponível","Versión no disponible") : message;
        add(status,text(checking?"Checking for updates…":heading,21,
                "Update available".equals(message)?PortUi.GREEN:PortUi.TEXT,true),8);
        add(status,text(UiText.t("Installed version")+": "+version+"  •  "+UiText.t("Build")+" "+code,12,PortUi.MUTED,false),10);
        if(checking) { ProgressBar bar=new ProgressBar(this,null,android.R.attr.progressBarStyleHorizontal);
            bar.setIndeterminate(true);bar.setIndeterminateTintList(android.content.res.ColorStateList.valueOf(PortUi.GOLD));
            LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,dp(5));p.topMargin=dp(15);status.addView(bar,p); }
        Button check=PortUi.button(this,"Check again",true);check.setEnabled(!checking);add(status,check,15);
        check.setOnClickListener(v->check());
        if(release!=null&&!checking) {
            LinearLayout detail=card();detail.addView(text(release.name,18,PortUi.TEXT,true));
            add(detail,text(release.tag+(release.prerelease?"  •  "+UiText.t("PRE-RELEASE"):""),12,PortUi.GOLD,false),8);
            if(release.buildCode>0)add(detail,text(UiText.t("Build")+" "+release.buildCode,12,PortUi.MUTED,false),8);
            add(detail,text(UiText.t("Published")+": "+release.published.substring(0,Math.min(10,release.published.length())),12,PortUi.MUTED,false),8);
            if("Update available".equals(message))action(detail,"Download APK",true,()->open(release.apk));
            if("Build information unavailable".equals(message))add(detail,text(UiText.tr("This release has no comparable version tag. Open GitHub to review it.",
                    "Esta release não tem uma tag de versão comparável. Confira no GitHub.",
                    "Esta publicación no tiene una etiqueta de versión comparable. Revísala en GitHub."),13,PortUi.MUTED,false),12);
            action(detail,"View release",false,()->open(release.page));
            // Release notes remain plain selectable text. Do not run embedded HTML or scripts.
            if(!release.notes.trim().isEmpty())action(detail,"Release notes",false,
                    ()->showDocument("Release notes",release.notes));
        }
        LinearLayout hub=card();hub.addView(text("Updates by contatg",15,PortUi.TEXT,true));
        add(hub,text("Downloads open in your browser. Install the APK over the existing app to keep your data.",13,PortUi.MUTED,false),8);
        action(hub,"Open GitHub releases",false,()->open(ReleaseUpdate.RELEASES));
    }
    private void check() {
        if(checking)return;checking=true;checked=true;release=null;final int token=++request;render();
        worker.execute(()->{
            ReleaseUpdate result=null;String error=null;
            try {result=GitHubUpdates.check();}
            catch(GitHubUpdates.LookupException e) {error=e.label;}
            catch(Exception e) {error="Could not check updates. Check your connection and try again.";}
            final ReleaseUpdate found=result;final String failure=error;
            runOnUiThread(()->{
                if(isFinishing()||isDestroyed()||token!=request)return;
                checking=false;release=found;message=failure!=null?failure:
                        found==null?"No published APK release found.":found.status(version);
                if(updates&&!document)render();
            });
        });
    }
    private void licenses() {
        try(InputStream in=getAssets().open("about/THIRD-PARTY-NOTICES.txt");ByteArrayOutputStream out=new ByteArrayOutputStream()) {
            byte[] b=new byte[4096];int n;while((n=in.read(b))!=-1)out.write(b,0,n);
            showDocument("Third-party licenses",out.toString("UTF-8"));
        } catch(Exception e) {Toast.makeText(this,UiText.t("License notices could not be opened"),Toast.LENGTH_LONG).show();}
    }
    private void showDocument(String title,String value) {
        document=true;content.removeAllViews();LinearLayout body=card();body.addView(text(title,20,PortUi.GOLD,true));
        TextView info=text(value,14,PortUi.TEXT,false);info.setTextIsSelectable(true);info.setLineSpacing(dp(4),1);
        add(body,info,16);action(body,"Back",false,this::render);scroll.scrollTo(0,0);
    }
    private void open(String url) {
        try {startActivity(new Intent(Intent.ACTION_VIEW,Uri.parse(url)));}
        catch(Exception e) {Toast.makeText(this,UiText.t("No browser is available."),Toast.LENGTH_LONG).show();}
    }
    private void goBack() { if(document)render();else finish(); }
    @Override public void onBackPressed() {goBack();}
    @Override protected void onSaveInstanceState(Bundle out) {out.putBoolean("updates",updates);super.onSaveInstanceState(out);}
    @Override protected void onDestroy() {++request;worker.shutdownNow();super.onDestroy();}
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if(focused)getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN|
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY|View.SYSTEM_UI_FLAG_LAYOUT_STABLE|
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }
}
