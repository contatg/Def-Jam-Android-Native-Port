package org.defjamffny.android;
import android.app.*;
import android.content.*;
import android.widget.*;
import android.view.*;
/** ZeroTier transports packets; a separately running own-room server provides rooms. */
final class ZeroTier148 {
 static String tr(String en,String pt,String es){return UiText.isPortuguese()?pt:UiText.isSpanish()?es:en;}
 static void migrate(Context c){android.content.SharedPreferences p=c.getSharedPreferences("online",0);if(!p.getBoolean("zerotier148",false)){String old=p.getString("server","");p.edit().putString("server",OnlineMenuState.communityServer(old)?"":old).putBoolean("custom_relay_enabled114",true).putBoolean("zerotier148",true).apply();}}
 static String name(Context c){String n=c.getSharedPreferences("online",0).getString("local_name148","Player");return OnlineMenuState.validName(n)?n:"Player";}
 static boolean ready(Context c){migrate(c);String s=c.getSharedPreferences("online",0).getString("server","");return OnlineMenuState.validServer(s)&&!OnlineMenuState.communityServer(s);}
 static void enter(Activity a,Runnable next){if(ready(a))next.run();else show(a,next);}
 static void show(Activity a,Runnable next){
  migrate(a);android.content.SharedPreferences p=a.getSharedPreferences("online",0);
  int pad=PortUi.dp(a,20);
  LinearLayout panel=new LinearLayout(a);panel.setOrientation(LinearLayout.VERTICAL);
  panel.setPadding(pad,pad,pad,pad);panel.setFocusableInTouchMode(true);
  TextView tag=PortUi.text(a,"CONTATG / MULTIPLAYER",10,PortUi.GOLD,true);
  tag.setLetterSpacing(.12f);panel.addView(tag);
  TextView title=PortUi.text(a,tr("Your private arena","Sua arena particular","Tu arena privada"),24,PortUi.TEXT,true);
  add(panel,title,6);
  TextView subtitle=PortUi.text(a,"ONLINE / ZEROTIER",11,PortUi.MUTED,true);add(panel,subtitle,5);
  ScrollView scroll=new ScrollView(a){
   @Override protected void onMeasure(int w,int h){
    int available=MeasureSpec.getMode(h)==MeasureSpec.UNSPECIFIED?getResources().getDisplayMetrics().heightPixels:MeasureSpec.getSize(h);
    super.onMeasure(w,MeasureSpec.makeMeasureSpec(Math.min(available,Math.max(PortUi.dp(a,80),Math.min(PortUi.dp(a,260),getResources().getDisplayMetrics().heightPixels-PortUi.dp(a,240)))),MeasureSpec.AT_MOST));
   }
  };
  scroll.setOverScrollMode(View.OVER_SCROLL_NEVER);scroll.setVerticalScrollBarEnabled(false);
  LinearLayout body=new LinearLayout(a);body.setOrientation(LinearLayout.VERTICAL);
  body.setPadding(0,0,0,PortUi.dp(a,8));scroll.addView(body);add(panel,scroll,14);
  TextView help=PortUi.text(a,tr("Connect every device to the same authorized ZeroTier network. Your PC must be running the room server.","Conecte os aparelhos à mesma rede ZeroTier autorizada. O servidor de salas precisa estar aberto no PC.","Conecta todos los dispositivos a la misma red ZeroTier autorizada. El servidor de salas debe estar abierto en el PC."),13,PortUi.MUTED,false);
  body.addView(help);
  add(body,PortUi.text(a,tr("SERVER ADDRESS","ENDEREÇO DO SERVIDOR","DIRECCIÓN DEL SERVIDOR"),10,PortUi.GOLD,true),16);
  EditText host=field(a,"10.0.0.2:27777",p.getString("server",""));add(body,host,6);
  host.setInputType(android.text.InputType.TYPE_CLASS_TEXT|android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
  host.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_NEXT|android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI);
  add(body,PortUi.text(a,tr("PLAYER NAME","NOME DO JOGADOR","NOMBRE DEL JUGADOR"),10,PortUi.GOLD,true),14);
  EditText nick=field(a,tr("Your nickname","Seu apelido","Tu apodo"),name(a));add(body,nick,6);
  nick.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(24)});
  nick.setInputType(android.text.InputType.TYPE_CLASS_TEXT|android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
  nick.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_DONE|android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI);
  Button open=PortUi.button(a,tr("Open ZeroTier app  ↗","Abrir aplicativo ZeroTier  ↗","Abrir aplicación ZeroTier  ↗"),false);add(body,open,12);
  open.setOnClickListener(v->{android.content.Intent i=a.getPackageManager().getLaunchIntentForPackage("com.zerotier.one");if(i!=null)a.startActivity(i);else Toast.makeText(a,tr("Install and connect ZeroTier One first.","Instale e conecte o ZeroTier One primeiro.","Instala y conecta ZeroTier One primero."),Toast.LENGTH_LONG).show();});
  add(body,PortUi.text(a,tr("Changes apply on your next connection. Use the PC IP:port, not the ZeroTier network ID.","As mudanças valem na próxima conexão. Use o IP:porta do PC, não o ID da rede ZeroTier.","Los cambios se aplican en la próxima conexión. Usa la IP:puerto del PC, no el ID de red ZeroTier."),11,PortUi.MUTED,false),10);
  View line=new View(a);line.setBackgroundColor(PortUi.LINE);LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,PortUi.dp(a,1));lp.topMargin=PortUi.dp(a,12);panel.addView(line,lp);
  LinearLayout actions=new LinearLayout(a);
  Button cancel=PortUi.button(a,"Cancel",false),save=PortUi.button(a,tr("Save connection","Salvar conexão","Guardar conexión"),true);
  actions.addView(cancel,new LinearLayout.LayoutParams(0,PortUi.dp(a,44),1));
  LinearLayout.LayoutParams saveParams=new LinearLayout.LayoutParams(0,PortUi.dp(a,44),1);saveParams.leftMargin=PortUi.dp(a,8);actions.addView(save,saveParams);add(panel,actions,12);
  AlertDialog d=PortUi.dialog(a).setView(panel).create();d.show();
  if(d.getWindow()!=null){
   d.getWindow().setBackgroundDrawable(PortUi.shape(a,PortUi.BG,PortUi.LINE,22));
   d.getWindow().setLayout(Math.min(PortUi.dp(a,600),a.getResources().getDisplayMetrics().widthPixels-PortUi.dp(a,32)),WindowManager.LayoutParams.WRAP_CONTENT);
   d.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN|WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
  }
  panel.requestFocus();cancel.setOnClickListener(v->d.dismiss());
  save.setOnClickListener(v->{String server=host.getText().toString().trim(),n=nick.getText().toString().trim();if(!OnlineMenuState.validServer(server)||OnlineMenuState.communityServer(server)){host.setError(tr("Enter your own server IP:port","Digite o IP:porta do seu servidor","Introduce la IP:puerto de tu servidor"));return;}if(!OnlineMenuState.validName(n)){nick.setError(tr("Use 1–24 letters/numbers","Use 1–24 letras/números","Usa 1–24 letras/números"));return;}p.edit().putString("server",server).putString("local_name148",n).putBoolean("custom_relay_enabled114",true).apply();d.dismiss();if(next!=null)next.run();});
 }
 private static void add(LinearLayout parent,View child,int margin){LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,-2);lp.topMargin=PortUi.dp(parent.getContext(),margin);parent.addView(child,lp);}
 private static EditText field(Activity a,String hint,String value){
  EditText input=new EditText(a);input.setSingleLine(true);input.setTextSize(15);
  input.setTextColor(PortUi.TEXT);input.setHintTextColor(PortUi.MUTED);
  input.setBackground(PortUi.shape(a,PortUi.CARD,PortUi.LINE,12));input.setBackgroundTintList(null);
  input.setOnFocusChangeListener((v,focused)->input.setBackground(PortUi.shape(a,PortUi.CARD,focused?PortUi.GOLD:PortUi.LINE,12)));
  int p=PortUi.dp(a,13);input.setPadding(p,p,p,p);input.setHint(hint);input.setText(value);
  return input;
 }
 private ZeroTier148(){}
}
