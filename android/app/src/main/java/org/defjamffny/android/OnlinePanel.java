package org.defjamffny.android;

import android.app.Dialog;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.*;
import android.text.InputFilter;
import android.view.*;
import android.view.inputmethod.InputMethodManager;
import android.widget.*;
import org.json.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;

/** Touch-first Android lobby. The original guest ONLINE entry is unchanged. */
final class OnlinePanel {
    private static native int nativeCommand(int action,String text,String extra,int value);
    private static native String nativeSnapshot();
    private static native void nativeAbort();
    private static native boolean nativeCanOpen();
    private static native boolean nativeFailed();
    private static native int nativeMenuEvent();
    private final DefJamActivity activity;
    private final ExecutorService worker=Executors.newSingleThreadExecutor();
    private final Handler ui=new Handler(Looper.getMainLooper());
    private final AtomicBoolean polling=new AtomicBoolean(),pending=new AtomicBoolean();
    private volatile int generation;
    private volatile boolean destroyed;
    private Dialog dialog;
    private LinearLayout root,rooms,players,votes,createSecret,joinSecret;
    private FrameLayout content;
    private final View[] pages=new View[7];
    private TextView stage,status,badge,roomCode,empty,privateHint;
    private EditText code,roomName,createPass,joinPass;
    private CheckBox privateRoom,protectedJoin;
    private Spinner fighter;
    private Button connect,create,quick,join,refresh,ready,back,disconnect;
    private ProgressBar progress;
    private JSONArray roster=new JSONArray();
    private int state,phase,shown=-1,form=-1;
    private boolean originalMenu,used,starting,inRoom,menuReady,localReady,passwordRooms;
    private String rosterKey="",roomKey="",playerKey="",voteKey="",notice="";
    OnlinePanel(DefJamActivity a){activity=a;}
    boolean hasOriginalMenu(){return originalMenu;}
    boolean isSessionRequested(){return used||DefJamActivity.nativeOnlineSession();}
    private String t(String s){return UiText.t(s);}
    private int dp(float n){return PortUi.dp(activity,n);}
    private void say(String s){Toast.makeText(activity,t(s),Toast.LENGTH_LONG).show();}
    void pollMenuRequest(){
        if(destroyed||!activity.hasWindowFocus())return;
        try{int bits=nativeMenuEvent();boolean available=(bits&1)!=0;
            if(available!=originalMenu){originalMenu=available;activity.restoreAfterOnline();}
            if((bits&2)!=0&&dialog==null)activity.openNativeOnline();
        }catch(UnsatisfiedLinkError ignored){}
    }
    void showWhenReady(){ui.postDelayed(new Runnable(){public void run(){
        if(destroyed||activity.isFinishing())return;
        try{if(activity.hasWindowFocus()&&nativeCanOpen()){activity.openNativeOnline();return;}}catch(UnsatisfiedLinkError e){return;}
        ui.postDelayed(this,1000);
    }},1000);}
    private final Runnable monitor=new Runnable(){public void run(){
        if(!used||destroyed)return;
        try{if(nativeFailed()){say("Online connection ended. Return to the main menu to reconnect.");disconnect();return;}}
        catch(UnsatisfiedLinkError e){return;}ui.postDelayed(this,1000);
    }};
    private LinearLayout column(){LinearLayout c=new LinearLayout(activity);c.setOrientation(1);return c;}
    private LinearLayout row(){LinearLayout c=new LinearLayout(activity);c.setOrientation(0);c.setGravity(Gravity.CENTER_VERTICAL);return c;}
    private void add(LinearLayout c,View v){LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,-2);p.topMargin=dp(8);c.addView(v,p);}
    private TextView text(String s,int size,int color,boolean bold){return PortUi.text(activity,s,size,color,bold);}
    private void caption(LinearLayout c,String s){add(c,text(s,13,PortUi.MUTED,false));}
    private LinearLayout card(String title){LinearLayout c=column();c.setPadding(dp(14),dp(8),dp(14),dp(14));c.setBackground(PortUi.shape(activity,PortUi.CARD,PortUi.LINE,14));add(c,text(title,18,PortUi.TEXT,true));return c;}
    private Button button(String title,boolean primary,Runnable click){
        Button b=PortUi.button(activity,title,primary);b.setMinHeight(dp(48));b.setMinimumHeight(dp(48));b.setFocusable(true);
        StateListDrawable bg=new StateListDrawable();
        bg.addState(new int[]{-android.R.attr.state_enabled},PortUi.shape(activity,0xff1a222d,PortUi.LINE,12));
        bg.addState(new int[]{android.R.attr.state_pressed},PortUi.shape(activity,0xff4c3d20,PortUi.GOLD,12));
        bg.addState(new int[]{android.R.attr.state_focused},PortUi.shape(activity,0xff4c3d20,PortUi.GOLD,12));
        bg.addState(new int[]{},PortUi.shape(activity,primary?PortUi.GOLD:0xff202a37,primary?0:PortUi.LINE,12));
        b.setBackground(bg);
        b.setTextColor(new android.content.res.ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled},new int[]{android.R.attr.state_focused},new int[]{android.R.attr.state_pressed},new int[]{}},new int[]{0xff758090,PortUi.TEXT,PortUi.TEXT,primary?PortUi.BG:PortUi.TEXT}));
        b.setOnClickListener(v->click.run());return b;
    }
    private Button action(LinearLayout c,String title,boolean primary,Runnable click){Button b=button(title,primary,click);add(c,b);return b;}
    private EditText field(LinearLayout c,String title,String value,int max,boolean secret){
        caption(c,title);EditText e=new EditText(activity);e.setSingleLine(true);e.setTextSize(16);e.setTextColor(PortUi.TEXT);e.setHintTextColor(PortUi.MUTED);e.setText(value);
        e.setFilters(new InputFilter[]{new InputFilter.LengthFilter(max)});e.setMinHeight(dp(48));e.setPadding(dp(12),dp(8),dp(12),dp(8));e.setBackground(PortUi.shape(activity,PortUi.BG,PortUi.LINE,10));e.setContentDescription(t(title));
        e.setInputType(android.text.InputType.TYPE_CLASS_TEXT|(secret?android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD:android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS));
        if(secret)e.setSaveEnabled(false);add(c,e);return e;
    }
    private ScrollView scroll(LinearLayout c){ScrollView s=new ScrollView(activity);s.setClipToPadding(false);s.setPadding(0,0,0,dp(8));s.addView(c);return s;}
    private View columns(LinearLayout a,LinearLayout b){
        if(activity.getResources().getDisplayMetrics().widthPixels/activity.getResources().getDisplayMetrics().density<560){LinearLayout c=column();add(c,a);add(c,b);return scroll(c);}
        LinearLayout r=row();r.setGravity(Gravity.TOP);LinearLayout.LayoutParams x=new LinearLayout.LayoutParams(0,-1,1),y=new LinearLayout.LayoutParams(0,-1,1);y.leftMargin=dp(12);r.addView(scroll(a),x);r.addView(scroll(b),y);return r;
    }
    private void weighted(LinearLayout c,View v,boolean gap){LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(0,-2,1);if(gap)p.leftMargin=dp(8);c.addView(v,p);}
    private void keyboard(){InputMethodManager m=(InputMethodManager)activity.getSystemService(android.content.Context.INPUT_METHOD_SERVICE);if(m!=null&&root!=null)m.hideSoftInputFromWindow(root.getWindowToken(),0);}
    void show(){
        if(destroyed||dialog!=null)return;
        try{if(!nativeCanOpen()){say("Open Online from the game's main menu.");return;}}catch(UnsatisfiedLinkError e){say("Online native module is unavailable in this APK.");return;}
        generation++;starting=false;shown=form=-1;state=phase=0;inRoom=localReady=passwordRooms=false;menuReady=true;roster=new JSONArray();rosterKey=roomKey=playerKey=voteKey=notice="";
        Dialog d=new Dialog(activity,android.R.style.Theme_Material_NoActionBar){
            @Override public void onBackPressed(){backAction();}
            @Override public boolean dispatchKeyEvent(KeyEvent e){
                int k=e.getKeyCode();boolean a=k==KeyEvent.KEYCODE_BUTTON_A,b=k==KeyEvent.KEYCODE_BUTTON_B||k==KeyEvent.KEYCODE_BACK||k==KeyEvent.KEYCODE_ESCAPE;
                if(a||b){if(e.getAction()==KeyEvent.ACTION_UP){if(b)backAction();else confirm();}return true;}return super.dispatchKeyEvent(e);
            }
            @Override public boolean dispatchGenericMotionEvent(MotionEvent e){return stick(e)||super.dispatchGenericMotionEvent(e);}
        };dialog=d;d.setCanceledOnTouchOutside(false);
        root=column();root.setPadding(dp(16),dp(10),dp(16),dp(10));root.setBackgroundColor(PortUi.BG);root.setFocusableInTouchMode(true);
        LinearLayout header=row(),title=column();title.addView(text("CONTATG / DEF JAM",10,PortUi.MUTED,true));title.addView(text("ONLINE",24,PortUi.TEXT,true));
        header.addView(title,new LinearLayout.LayoutParams(0,-2,1));badge=text("OFFLINE",11,PortUi.GOLD,true);header.addView(badge);
        Button close=button("Close online",false,d::dismiss);LinearLayout.LayoutParams cp=new LinearLayout.LayoutParams(-2,dp(48));cp.leftMargin=dp(12);header.addView(close,cp);root.addView(header);
        stage=text("Connection",12,PortUi.GOLD,true);add(root,stage);content=new FrameLayout(activity);LinearLayout.LayoutParams fill=new LinearLayout.LayoutParams(-1,0,1);fill.topMargin=dp(10);root.addView(content,fill);buildPages();
        LinearLayout detail=row();progress=new ProgressBar(activity);detail.addView(progress,new LinearLayout.LayoutParams(dp(18),dp(18)));status=text("Not connected.",12,PortUi.MUTED,false);status.setMaxLines(2);LinearLayout.LayoutParams sp=new LinearLayout.LayoutParams(0,-2,1);sp.leftMargin=dp(8);detail.addView(status,sp);add(root,detail);
        LinearLayout footer=row();back=button("Back to game",false,this::backAction);disconnect=button("Disconnect",false,this::disconnect);weighted(footer,back,false);weighted(footer,disconnect,true);add(root,footer);
        d.setContentView(root);d.setOnDismissListener(v->{if(dialog!=d)return;keyboard();createPass.setText("");joinPass.setText("");dialog=null;ui.removeCallbacks(poll);command(9,"","",0);
            if(!starting){generation++;used=false;abort();command(8,"","",0);}else{ui.removeCallbacks(monitor);ui.post(monitor);}activity.restoreAfterOnline();
        });
        d.show();Window w=d.getWindow();if(w!=null){w.setBackgroundDrawable(new ColorDrawable(PortUi.BG));w.setLayout(-1,-1);
            w.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE|WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN);
            if(Build.VERSION.SDK_INT>=28){WindowManager.LayoutParams a=w.getAttributes();a.layoutInDisplayCutoutMode=WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;w.setAttributes(a);}
            w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN|View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
            root.setOnApplyWindowInsetsListener((v,i)->{int l=i.getSystemWindowInsetLeft(),r=i.getSystemWindowInsetRight();if(Build.VERSION.SDK_INT>=28&&i.getDisplayCutout()!=null){l=Math.max(l,i.getDisplayCutout().getSafeInsetLeft());r=Math.max(r,i.getDisplayCutout().getSafeInsetRight());}int bottom=Math.min(i.getSystemWindowInsetBottom(),dp(48));if(Build.VERSION.SDK_INT>=30)bottom=i.getInsets(WindowInsets.Type.systemBars()|WindowInsets.Type.displayCutout()).bottom;v.setPadding(dp(16)+l,dp(10)+i.getSystemWindowInsetTop(),dp(16)+r,dp(10)+bottom);return i;});root.requestApplyInsets();
        }
        root.requestFocus();setPage(0);refreshActions();command(9,"","",1);poll.run();
    }
    private void buildPages(){
        LinearLayout profile = card("FFNY Online");
        caption(profile, UiText.tr("Server region", "Região do servidor", "Región del servidor"));
        Spinner region = new Spinner(activity);
        String[] relays = {OnlineMenuState.DEFAULT_SERVER, OnlineMenuState.NA_SERVER, OnlineMenuState.WEST_SERVER};
        ArrayAdapter<String> regions = new ArrayAdapter<>(activity, android.R.layout.simple_spinner_item,
                new String[] {"EU", "NA West", "US West"});
        regions.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        region.setAdapter(regions);
        region.setMinimumHeight(dp(48));
        add(profile, region);
        connect = action(profile, UiText.tr("Sign in with Discord", "Entrar com Discord", "Entrar con Discord"),
            true, () -> OnlineAccount133.enter(activity, () -> {
                if (dialog == null) return;
                String player = OnlineAccount133.gameName();
                if (!OnlineMenuState.validName(player)) return;
                used = true;
                keyboard();
                command(0, relays[region.getSelectedItemPosition()], player, 0);
            }));
        pages[0] = scroll(profile);
        LinearLayout actions=card("Play with friends");create=action(actions,"Create room",true,()->{form=5;setPage(5);});action(actions,"Join by code",false,()->{form=6;setPage(6);});quick=action(actions,"Quick match • 1 vs 1",false,()->command(2,"","",0));
        LinearLayout available=card("Public rooms");refresh=action(available,"Refresh rooms",false,()->command(1,"","",0));empty=text("No public rooms yet. Create one or join by code.",13,PortUi.MUTED,false);add(available,empty);rooms=column();add(available,rooms);pages[1]=columns(actions,available);
        LinearLayout room=card("Your room");roomCode=text("",26,PortUi.GOLD,true);add(room,roomCode);privateHint=text("",12,PortUi.MUTED,false);add(room,privateHint);players=column();add(room,players);caption(room,"Share this code with your friend. Both players must be ready.");
        LinearLayout selection=card("Choose your fighter");fighter=new Spinner(activity,Spinner.MODE_DROPDOWN);fighter.setMinimumHeight(dp(48));fighter.setPopupBackgroundDrawable(PortUi.shape(activity,PortUi.CARD,PortUi.LINE,12));fighter.setBackground(PortUi.shape(activity,PortUi.BG,PortUi.LINE,10));add(selection,fighter);
        ready=action(selection,"Ready to fight",true,()->{JSONObject f=roster.optJSONObject(fighter.getSelectedItemPosition());if(f!=null)command(5,"","",f.optInt("id",-1));});caption(selection,"Get ready to vote for the arena.");pages[2]=columns(room,selection);
        LinearLayout voting=card("Choose the arena");caption(voting,"Tap an arena to vote. The match starts when voting ends.");votes=column();add(voting,votes);pages[3]=scroll(voting);
        LinearLayout wait=card("Starting match");caption(wait,"Preparing the fighters and arena…");add(wait,new ProgressBar(activity));pages[4]=scroll(wait);
        LinearLayout creating=card("Create room");roomName=field(creating,"Room name",t("My room"),24,false);privateRoom=new CheckBox(activity);privateRoom.setText(t("Private room • password required"));privateRoom.setTextColor(PortUi.TEXT);privateRoom.setMinimumHeight(dp(48));add(creating,privateRoom);
        createSecret=column();createPass=field(createSecret,"Password • 6 to 64 characters","",64,true);add(creating,createSecret);createSecret.setVisibility(View.GONE);privateRoom.setOnCheckedChangeListener((b,on)->createSecret.setVisibility(on&&passwordRooms?View.VISIBLE:View.GONE));caption(creating,"Public rooms appear in the list and do not need a password.");caption(creating,"On FFNY Online, code-only rooms are hidden but have no password. Share the code privately.");
        action(creating,"Create room",true,()->{String title=roomName.getText().toString().trim(),pass=privateRoom.isChecked()&&passwordRooms?createPass.getText().toString():"";if(!OnlineMenuState.validName(title)){say("Use a room name with 1 to 24 letters or numbers.");return;}if(privateRoom.isChecked()&&passwordRooms&&!OnlineMenuState.validPassword(pass)){say("Use a password with 6 to 64 characters.");return;}keyboard();command(3,title,pass,OnlineMenuState.roomMode(privateRoom.isChecked(),passwordRooms));});pages[5]=scroll(creating);
        LinearLayout entering=card("Join by code");code=field(entering,"Room code","",6,false);code.setFilters(new InputFilter[]{new InputFilter.LengthFilter(6),new InputFilter.AllCaps()});
        protectedJoin=new CheckBox(activity);protectedJoin.setText(t("This room has a password"));protectedJoin.setTextColor(PortUi.TEXT);protectedJoin.setMinimumHeight(dp(48));add(entering,protectedJoin);
        joinSecret=column();joinPass=field(joinSecret,"Room password","",64,true);add(entering,joinSecret);joinSecret.setVisibility(View.GONE);protectedJoin.setOnCheckedChangeListener((b,on)->joinSecret.setVisibility(on&&passwordRooms?View.VISIBLE:View.GONE));
        join=action(entering,"Join room",true,()->{String key=code.getText().toString().trim(),pass=protectedJoin.isChecked()&&passwordRooms?joinPass.getText().toString():"";if(!OnlineMenuState.validCode(key)){say("Enter a six-character room code.");return;}if(protectedJoin.isChecked()&&passwordRooms&&!OnlineMenuState.validPassword(pass)){say("Use a password with 6 to 64 characters.");return;}keyboard();command(4,key,pass,0);});pages[6]=scroll(entering);roomOptions();
        for(View page:pages){page.setVisibility(View.GONE);content.addView(page,new FrameLayout.LayoutParams(-1,-1));}
    }
    private void roomOptions(){
        privateRoom.setChecked(false);privateRoom.setText(t(passwordRooms?"Private room • password required":"By code only • no password"));createPass.setText("");createSecret.setVisibility(View.GONE);
        protectedJoin.setChecked(false);protectedJoin.setVisibility(passwordRooms?View.VISIBLE:View.GONE);joinPass.setText("");joinSecret.setVisibility(View.GONE);
    }
    private void setPage(int page){
        if(page==shown)return;shown=page;for(int i=0;i<pages.length;i++)pages[i].setVisibility(i==page?View.VISIBLE:View.GONE);
        stage.setText(t(new String[]{"01 / Connection","02 / Rooms","03 / Your room","04 / Arena vote","Starting match","Create room","Join by code"}[page]));if(!root.isInTouchMode())defaultFocus();
    }
    private void setText(TextView v,String s){if(!s.contentEquals(v.getText()))v.setText(s);}
    private void refreshActions(){
        if(dialog==null)return;boolean linked=OnlineMenuState.linked(state)&&used,busy=pending.get();
        connect.setEnabled(menuReady&&!busy&&(state==5||(state==0&&!used)));
        create.setEnabled(linked&&!inRoom&&!busy);quick.setEnabled(create.isEnabled());refresh.setEnabled(create.isEnabled());join.setEnabled(create.isEnabled());
        ready.setEnabled(linked&&inRoom&&phase==0&&roster.length()>0&&!busy);fighter.setEnabled(ready.isEnabled()&&!localReady);ready.setText(t(localReady?"Ready • tap to cancel":"Ready to fight"));
        for(int i=0;i<rooms.getChildCount();i++){View row=rooms.getChildAt(i);row.setEnabled(create.isEnabled()&&Boolean.TRUE.equals(row.getTag()));}for(int i=0;i<votes.getChildCount();i++)votes.getChildAt(i).setEnabled(linked&&inRoom&&phase==1&&!busy);
        back.setText(t(form>=5?"Back":inRoom?"Leave room":"Back to game"));back.setEnabled(!busy);disconnect.setVisibility(used||linked?View.VISIBLE:View.GONE);
        progress.setVisibility(busy||state==1||state==3?View.VISIBLE:View.GONE);badge.setText(t(linked?"CONNECTED":used&&state!=5?"CONNECTING":"OFFLINE"));badge.setTextColor(linked?PortUi.GREEN:state==5?PortUi.RED:PortUi.GOLD);
    }
    private void command(int action,String text,String extra,int value){
        if(destroyed)return;boolean interactive=action>=0&&action<=7;if(interactive&&!pending.compareAndSet(false,true))return;
        final int epoch=generation;final Dialog expected=dialog;if(interactive){notice="";refreshActions();}
        worker.execute(()->{String error="";
            try{if(action!=8&&(destroyed||epoch!=generation))return;
                String host=action==0&&text.isEmpty()?OnlineServerDirectory.resolve():text;
                String secret=(action==3||action==4)&&!extra.isEmpty()?OnlineServerDirectory.credential(extra):extra;
                if(action!=8&&(destroyed||epoch!=generation))return;
                if(nativeCommand(action,host,secret,value)<0)error=t("Action unavailable. Check the connection and main menu.");
            }catch(UnsatisfiedLinkError e){error=t("Online native module is unavailable in this APK.");}catch(Exception e){error=t(e.getMessage()==null?"Connection failed.":e.getMessage());}
            finally{if(interactive)pending.set(false);final String message=error;
                ui.post(()->{if(destroyed||epoch!=generation||dialog!=expected)return;if(!message.isEmpty()){notice=message;if(action==0)used=false;setText(status,notice);}refreshActions();});
            }
        });
    }
    private final Runnable poll=new Runnable(){public void run(){
        if(dialog==null||destroyed)return;final Dialog expected=dialog;final int epoch=generation;
        if(polling.compareAndSet(false,true))worker.execute(()->{JSONObject data=null;
            try{if(!destroyed&&epoch==generation)data=new JSONObject(nativeSnapshot());}catch(Exception|UnsatisfiedLinkError ignored){}finally{polling.set(false);}
            final JSONObject snapshot=data;ui.post(()->{if(destroyed||dialog!=expected||epoch!=generation)return;if(snapshot!=null)render(snapshot);else setText(status,t("Could not read the online connection. Try reconnecting."));});
        });ui.postDelayed(this,500);
    }};
    private void render(JSONObject data){
        state=data.optInt("state");menuReady=data.optInt("menu")==1;JSONObject room=data.optJSONObject("room");if(room==null)room=new JSONObject();boolean wasRoom=inRoom;inRoom=room.optInt("in_room")!=0;phase=room.optInt("phase");
        if(inRoom||!OnlineMenuState.linked(state))form=-1;if(inRoom&&!wasRoom){createPass.setText("");joinPass.setText("");notice="";}
        String error=data.optString("error","");if(!error.isEmpty()){notice=t(error);if(passwordRooms&&error.equals("Private room: password missing or incorrect.")){form=6;protectedJoin.setChecked(true);}}setText(status,notice.isEmpty()?data.optString("status",t("Not connected.")):notice);
        updateRoster(data.optJSONArray("fighters"));updateRooms(data.optJSONArray("rooms"));updatePlayers(room);updateVotes(room.optJSONArray("candidates"));
        setPage(form>=5?form:OnlineMenuState.page(state,inRoom,phase));refreshActions();
        if(OnlineMenuState.start(state,inRoom,phase,used,data.optInt("desync"))){starting=true;dialog.dismiss();}
    }
    private void updateRoster(JSONArray list){
        if(list==null)return;String key=list.toString();if(key.equals(rosterKey))return;rosterKey=key;JSONObject old=roster.optJSONObject(fighter.getSelectedItemPosition());int selected=old==null?-1:old.optInt("id",-1);roster=list;
        String[] names=new String[list.length()];int selectedIndex=0;for(int i=0;i<names.length;i++){JSONObject f=list.optJSONObject(i);names[i]=f==null?"—":f.optString("name","—");if(f!=null&&f.optInt("id",-1)==selected)selectedIndex=i;}
        ArrayAdapter<String> adapter=new ArrayAdapter<String>(activity,android.R.layout.simple_spinner_item,names){
            private View item(int i){TextView v=text(getItem(i),15,PortUi.TEXT,false);v.setPadding(dp(12),dp(10),dp(12),dp(10));v.setMinHeight(dp(48));v.setGravity(Gravity.CENTER_VERTICAL);v.setBackgroundColor(PortUi.CARD);return v;}
            @Override public View getView(int i,View v,ViewGroup parent){return item(i);}
            @Override public View getDropDownView(int i,View v,ViewGroup parent){return item(i);}
        };fighter.setAdapter(adapter);if(names.length>0)fighter.setSelection(selectedIndex);
    }
    private void updateRooms(JSONArray list){
        if(list==null)list=new JSONArray();String key=list.toString();if(key.equals(roomKey))return;roomKey=key;rooms.removeAllViews();empty.setVisibility(list.length()==0?View.VISIBLE:View.GONE);
        for(int i=0;i<list.length();i++){JSONObject r=list.optJSONObject(i);if(r==null)continue;String code=r.optString("code");Button b=button(r.optString("name")+" · "+r.optInt("players")+"/"+r.optInt("max")+"\n"+code,false,()->command(4,code,"",0));b.setTag(r.optInt("phase")==0&&r.optInt("players")<r.optInt("max"));b.setSingleLine(false);b.setMaxLines(2);add(rooms,b);}
    }
    private void updatePlayers(JSONObject room){
        setText(roomCode,room.optString("code","—"));setText(privateHint,t(room.optInt("public",1)==0?(passwordRooms?"Private room":"By code only • no password"):"Public room"));JSONArray list=room.optJSONArray("players");if(list==null)list=new JSONArray();int you=room.optInt("you",-2);localReady=false;
        for(int i=0;i<list.length();i++){JSONObject p=list.optJSONObject(i);if(p!=null&&p.optInt("slot",-1)==you)localReady=p.optInt("ready")!=0;}
        String key=you+":"+list.toString();if(key.equals(playerKey))return;playerKey=key;players.removeAllViews();
        for(int i=0;i<list.length();i++){JSONObject p=list.optJSONObject(i);if(p==null)continue;boolean on=p.optInt("ready")!=0;String title=p.optString("name")+(p.optInt("slot",-1)==you?" ("+t("You")+")":"")+"\n"+t(on?"Ready":"Choosing fighter");TextView v=text(title,15,on?PortUi.GREEN:PortUi.TEXT,true);v.setPadding(dp(12),dp(10),dp(12),dp(10));v.setBackground(PortUi.shape(activity,PortUi.BG,on?PortUi.GREEN:PortUi.LINE,10));add(players,v);}
    }
    private void updateVotes(JSONArray list){
        if(list==null)list=new JSONArray();String key=phase+":"+list.toString();if(key.equals(voteKey))return;voteKey=key;votes.removeAllViews();
        if(phase==1)for(int i=0;i<list.length();i++){JSONObject c=list.optJSONObject(i);if(c==null)continue;final int choice=i;action(votes,c.optString("name")+" · "+c.optInt("votes")+" "+t("votes"),false,()->command(6,"","",choice));}
    }
    private void backAction(){if(dialog==null)return;if(pending.get()&&(form>=5||inRoom))return;if(form>=5){form=-1;keyboard();setPage(1);refreshActions();}else if(inRoom)command(7,"","",0);else dialog.dismiss();}
    private View defaultButton(){if(shown==0)return connect;if(shown==1)return create;if(shown==2)return fighter;if(shown==3&&votes.getChildCount()>0)return votes.getChildAt(0);if(shown==5)return roomName;if(shown==6)return code;return back;}
    private void defaultFocus(){View v=defaultButton();if(v!=null&&v.isEnabled())v.requestFocus();}
    private void confirm(){if(dialog==null)return;View v=dialog.getCurrentFocus();if(v==null||v==root){defaultFocus();v=dialog.getCurrentFocus();}if(v!=null&&v.isEnabled())v.performClick();}
    private int stickDirection;private long stickAt;
    private boolean stick(MotionEvent e){
        if(!e.isFromSource(InputDevice.SOURCE_JOYSTICK)||e.getActionMasked()!=MotionEvent.ACTION_MOVE)return false;float x=e.getAxisValue(MotionEvent.AXIS_HAT_X),y=e.getAxisValue(MotionEvent.AXIS_HAT_Y);if(Math.abs(x)<.5f)x=e.getAxisValue(MotionEvent.AXIS_X);if(Math.abs(y)<.5f)y=e.getAxisValue(MotionEvent.AXIS_Y);
        int dir=Math.max(Math.abs(x),Math.abs(y))<.55f?0:Math.abs(x)>Math.abs(y)?(x>0?View.FOCUS_RIGHT:View.FOCUS_LEFT):(y>0?View.FOCUS_DOWN:View.FOCUS_UP);if(dir==0){stickDirection=0;return true;}long now=SystemClock.uptimeMillis();if(dir==stickDirection&&now-stickAt<220)return true;stickDirection=dir;stickAt=now;
        View v=dialog==null?null:dialog.getCurrentFocus(),next=v==null?null:v.focusSearch(dir);if(next!=null)next.requestFocus();else defaultFocus();return true;
    }
    private void abort(){try{nativeAbort();}catch(UnsatisfiedLinkError ignored){}}
    void disconnect(){generation++;boolean legacy=used;used=false;form=-1;ui.removeCallbacks(monitor);abort();if(legacy)command(8,"","",0);activity.restoreAfterOnline();}
    void suspend(){if(destroyed)return;generation++;abort();boolean wasUsed=used;used=false;ui.removeCallbacks(monitor);if(dialog!=null){starting=false;dialog.dismiss();}else if(wasUsed)command(8,"","",0);}
    void destroy(){suspend();destroyed=true;ui.removeCallbacksAndMessages(null);worker.shutdown();}
}
