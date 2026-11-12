// SPDX-License-Identifier: GPL-3.0-or-later
package org.defjamffny.android;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.Gravity;
import android.widget.TextView;
import java.io.File;

/** Relaunches in the UI process after the old emulation process has exited. */
public final class HakuXRestart36 extends Activity {
    static final String OLD_PID="hakux36_old_pid";
    private final Handler handler=new Handler(Looper.getMainLooper());
    private long deadline;
    private final Runnable waitForProcess=this::startWhenReady;
    @Override protected void onCreate(Bundle state){super.onCreate(state);TextView text=new TextView(this);
        text.setText("Reabrindo a emulação…");text.setTextColor(PortUi.TEXT);text.setBackgroundColor(PortUi.BG);
        text.setTextSize(18);text.setGravity(Gravity.CENTER);setContentView(text);
        deadline=SystemClock.uptimeMillis()+5000;handler.postDelayed(waitForProcess,80);}
    private void startWhenReady(){
        int old=getIntent().getIntExtra(OLD_PID,-1);
        if(old>0&&new File("/proc/"+old).exists()){
            if(SystemClock.uptimeMillis()>=deadline){android.widget.Toast.makeText(this,"Não foi possível reiniciar a emulação.",android.widget.Toast.LENGTH_LONG).show();finish();}
            else handler.postDelayed(waitForProcess,60);return;
        }
        Intent next=new Intent(this,HakuXGame36.class).putExtras(getIntent()).setData(getIntent().getData());
        if(getIntent().getClipData()!=null)next.setClipData(getIntent().getClipData());next.removeExtra(OLD_PID);
        next.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);startActivity(next);finish();
    }
    @Override protected void onDestroy(){handler.removeCallbacks(waitForProcess);super.onDestroy();}
}
