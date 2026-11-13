/* Android's game-styled graphics screen. Drawn by the existing Vulkan overlay,
 * not an Activity/dialog. No title files or simulation routines are modified. */
#if defined(__ANDROID__)
#include "graphics_menu145.h"
#include "graphics_model145.h"
#include "online_screen.h"
#include "pc_settings.h"
#include "recomp_settings.h"
#include "d3d8_quality145.h"
#include "platform/android_sched.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
std::atomic<int> request145{0},editor145{0},language145{0},legacy_hud145{1},hud145{-1},position145{0};
std::atomic<int> framegen29{0},hudStyle{0};
std::atomic<unsigned> restart148{0};std::atomic<int> closed148{0},active148{0};
std::atomic<long long> reopen148{0};double leaving148=0,armed148=0;
static long long clock148(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
graphics145::Draft draft145;
int page145=-1,selected145=0,leave145=0;bool confirm145=false,restart145=false;
bool from_game147=false,wait_input147=false;
std::string status145;
const char *word(const char *en,const char *pt,const char *es){int l=language145.load();return l==1?pt:l==2?es:en;}
int stored145(int id){
 const auto &r=graphics145::rows()[id];const RecompSetting *s=recomp_settings_find(r.section,r.key);int v=s?s->stored:r.def;
 if(id==graphics145::SCALE&&v==0){const RecompSetting *old=recomp_settings_find("display","render_scale");v=480*(old?old->stored:1);}
 if(id==graphics145::HUD&&v<0)v=legacy_hud145.load();
 if(id==graphics145::FXAA&&v<0){const char *e=getenv("DEFJAM_LEGACY_FXAA145");v=e&&e[0]=='1';}
 return graphics145::sanitized(id,v);
}
void read145(){for(int i=0;i<graphics145::COUNT;i++)draft145.load(i,stored145(i));}
std::string value145(int id){
 using namespace graphics145;int v=draft145.value[id];char b[96];
 if(id==PRESET){const char *en[]={"Custom","Performance","Balanced","Quality"},*pt[]={"Personalizado","Desempenho","Equilibrado","Qualidade"},*es[]={"Personalizado","Rendimiento","Equilibrado","Calidad"};return word(en[v],pt[v],es[v]);}
 if(id==SPATIAL){const char *en[]={"Original","Bicubic","Lanczos-2"},*pt[]={"Original","Bicúbico","Lanczos-2"},*es[]={"Original","Bicúbico","Lanczos-2"};return word(en[v],pt[v],es[v]);}
 if(id==FG_ENABLE)return v==1?"Win-FG v37":v==2?"Lossless / LSFG":word("Off","Desligado","Desactivado");
 if(id==STORY_SUBTITLES)return v==1?word("All scenes","Todas as cenas","Todas las escenas"):v==2?word("Original","Original","Original"):word("Automatic","Automático","Automático");
 if(id==FG_MULTIPLIER){snprintf(b,sizeof b,"x%d",v);return b;}
 if(id==FG_REFRESH){snprintf(b,sizeof b,"%d Hz",v);return b;}
 if(id==FG_FLOW){snprintf(b,sizeof b,"%d%%",v);return b;}
 if(id==SCALE){snprintf(b,sizeof b,v==2160?"2160p / 4K":"%dp",v);return b;}
 if(id==FPS){if(v==-1)return word("Auto 30 / 60","Auto 30 / 60","Auto 30 / 60");if(!v)return word("Original","Original","Original");snprintf(b,sizeof b,"%d FPS",v);return b;}
 if(id==ASPECT){const char *a[]={"4:3","Stretch","Automatic widescreen","16:9","","21:9"};if(v==1)return word("Stretch","Esticada","Estirada");if(v==2)return word("Automatic widescreen","Widescreen automático","Panorámico automático");return a[v];}
 if(id==BILINEAR)v=!v;
 if(id==ANISO){if(v==1)return word("Original","Original","Original");snprintf(b,sizeof b,"%dx",v);return b;}
 if(id==SHARPNESS){snprintf(b,sizeof b,"%d%%",100-v/2);return b;}
 if(id==HUD_STYLE)return v?word("Classic","Antigo","Clásico"):word("Compact","Compacto","Compacto");
 if(id==HUD_POSITION){const char *en[]={"Top right","Top center","Top left"},*pt[]={"Superior direito","Superior central","Superior esquerdo"},*es[]={"Superior derecho","Superior central","Superior izquierdo"};return word(en[v],pt[v],es[v]);}
 if(id==FILTER){const char *en[]={"Original","Sharpen","Vivid","Scanlines","CRT","Tone mapping (SDR)"},*pt[]={"Original","Nitidez","Cores vivas","Linhas de varredura","CRT","Mapeamento de tons"},*es[]={"Original","Nitidez","Colores vivos","Líneas de barrido","CRT","Mapeo de tonos"};return word(en[v],pt[v],es[v]);}
 return v?word("On","Ligado","Activado"):word("Off","Desligado","Desactivado");
}
const char *help145(int id){
 using namespace graphics145;
 if(id==FG_ENABLE||id==FG_MULTIPLIER||id==FG_FLOW)return word("Experimental optical flow; adds latency. Restart. Cannot combine with FSR1 or image filters.","Fluxo óptico experimental; aumenta a latência. Reinicie. Incompatível com FSR1 e filtros de imagem.","Flujo óptico experimental; añade latencia. Reinicia. Incompatible con FSR1 y filtros.");
 if(id==SPATIAL)return word("Bicubic / Lanczos-2 enlarge the image and cost GPU. Restart; FG and FSR1 must be off.","Bicúbico / Lanczos-2 ampliam a imagem e usam GPU. Reinicie; desligue FG e FSR1.","Bicúbico / Lanczos-2 amplían la imagen y usan GPU. Reinicia; desactiva FG y FSR1.");
 if(id==PRESET)return word("Performance keeps 1x and disables costly image effects. Existing automatic 30/60 pacing is retained.","Desempenho usa 1x e desliga efeitos de imagem caros. Mantém o ritmo automático 30/60.","Rendimiento usa 1x y desactiva efectos de imagen costosos. Mantiene el ritmo automático 30/60.");
 if(id==SCALE)return word("Higher resolution costs more GPU time. * Restart the game to apply.","Resolução maior exige mais GPU. * Reinicie o jogo para aplicar.","Una resolución mayor exige más GPU. * Reinicia el juego para aplicar.");
 if(id==FSR||id==FXAA||id==SHARPNESS)return word("FSR enlarges a lower-resolution picture and replaces FXAA while active. Optional GPU cost.","FSR amplia a imagem em baixa resolução e substitui o FXAA quando ativo. Usa mais GPU.","FSR amplía la imagen a baja resolución y sustituye a FXAA cuando está activo. Usa más GPU.");
 if(id==FG_REFRESH||(id==FPS&&draft145.value[FG_ENABLE]))return word("For x2, set 30 FPS at 60 Hz or 60 FPS at 120 Hz.","Para x2, limite em 30 FPS para 60 Hz ou 60 FPS para 120 Hz.","Para x2, limita a 30 FPS para 60 Hz o a 60 FPS para 120 Hz.");
 if(id==FPS)return word("Presentation limit only. Automatic keeps the existing 30/60 policy and game speed.","Limita a apresentação. Automático mantém a política 30/60 e a velocidade do jogo.","Limita la presentación. Automático mantiene la política 30/60 y la velocidad del juego.");
 if(rows()[id].restart)return word("* Saved for the next launch. Restart the game after applying.","* Salvo para a próxima abertura. Reinicie o jogo após aplicar.","* Guardado para el próximo inicio. Reinicia el juego después de aplicar.");
 return word("Use left / right to change. Apply to save; Back keeps the current settings.","Use esquerda / direita para alterar. Aplique para salvar; Voltar mantém as opções atuais.","Usa izquierda / derecha para cambiar. Aplica para guardar; Atrás mantiene las opciones actuales.");
}
bool apply145(){
 using namespace graphics145;bool pending=draft145.needs_restart();std::array<int,COUNT> before{};
 if((draft145.value[FG_ENABLE]&&(draft145.value[FSR]||draft145.value[FILTER]||draft145.value[SPATIAL])) || (draft145.value[FSR]&&draft145.value[SPATIAL])){
  status145=word("Choose FG, FSR1 or one spatial upscaler. FG also requires image filters off.","Escolha FG, FSR1 ou um ampliador espacial. Com FG, desligue também o filtro de imagem.","Elige FG, FSR1 o un ampliador espacial. Con FG, desactiva también el filtro de imagen.");return false;
 }
 for(int i=0;i<COUNT;i++){const RecompSetting *s=recomp_settings_find(rows()[i].section,rows()[i].key);before[i]=s?s->stored:rows()[i].def;}
 for(int i=0;i<COUNT;i++)if(recomp_settings_set(rows()[i].section,rows()[i].key,draft145.value[i])<0){
  for(int j=0;j<i;j++)recomp_settings_set(rows()[j].section,rows()[j].key,before[j]);
  status145=word("Could not apply settings.","Não foi possível aplicar.","No se pudo aplicar.");return false;
 }
 const char *path=pc_settings_path();
 if(!path||!*path||recomp_settings_save(path)!=0){
  for(int i=0;i<COUNT;i++)recomp_settings_set(rows()[i].section,rows()[i].key,before[i]);
  status145=word("Could not save. Check storage access.","Não foi possível salvar. Verifique o acesso aos arquivos.","No se pudo guardar. Revisa el acceso a archivos.");return false;
 }
 defjam_sched_set_limit145(draft145.value[FPS]);
 d3d8_vk_quality145(draft145.value[FSR],draft145.value[FXAA],draft145.value[FILTER],draft145.value[SHARPNESS]);
 hud145=draft145.value[HUD];position145=draft145.value[HUD_POSITION];hudStyle=draft145.value[HUD_STYLE];
 unsigned changes148=0;for(int i=0;i<COUNT;i++)if(rows()[i].restart&&before[i]!=draft145.value[i])changes148|=1u<<(i>=18?18:i);
 if(changes148){restart148.fetch_or(changes148);fprintf(stderr,"[GRAPHICS148] restart-required options mask=0x%X; saved before prompt\n",changes148);}
 draft145.accepted();restart145|=pending;
 status145=restart145?word("Saved. * Some options need a restart.","Salvo. * Algumas opções precisam de reinício.","Guardado. * Algunas opciones requieren reiniciar."):word("Saved and applied.","Salvo e aplicado.","Guardado y aplicado.");
 fprintf(stderr,"[GRAPHICS145] saved inside game; fps=%d fsr=%d fxaa=%d filter=%d restart=%d\n",draft145.value[FPS],draft145.value[FSR],draft145.value[FXAA],draft145.value[FILTER],restart145);
 return true;
}
struct Canvas145 {
 ImDrawList *dl;float scale,ox,oy;
 explicit Canvas145(ImVec2 size):dl(ImGui::GetForegroundDrawList()){
  scale=std::min(size.x/760.f,size.y/480.f);ox=(size.x-760*scale)*.5f;oy=(size.y-480*scale)*.5f;
 }
 ImVec2 p(float x,float y){return ImVec2(ox+x*scale,oy+y*scale);}
 void fill(float x,float y,float w,float h,ImU32 c){dl->AddRectFilled(p(x,y),p(x+w,y+h),c);}
 void line(float x,float y,float x2,float y2,ImU32 c,float n=1){dl->AddLine(p(x,y),p(x2,y2),c,n*scale);}
 void text(const char *s,float x,float y,float size,ImU32 c,int font=1,bool right=false){
  ImFont *f=online_screen::menu_font145(font);if(!f)f=ImGui::GetFont();float fs=size*scale;
  float width=f->CalcTextSizeA(fs,10000,0,s).x;ImVec2 at=p(x,y);if(right)at.x-=width;
  dl->AddText(f,fs,ImVec2(at.x+scale,at.y+scale),IM_COL32(0,0,0,240),s);dl->AddText(f,fs,at,c,s);
 }
 bool hit(float x,float y,float w,float h){ImVec2 a=p(x,y),b=p(x+w,y+h),m=ImGui::GetIO().MousePos;return m.x>=a.x&&m.x<b.x&&m.y>=a.y&&m.y<b.y;}
};
const ImU32 CREAM145=IM_COL32(242,230,200,255),GOLD145=IM_COL32(230,174,67,255),GREY145=IM_COL32(160,159,153,255),RED145=IM_COL32(174,16,39,255);
bool key145(ImGuiKey a,ImGuiKey b=ImGuiKey_None,bool repeat=false){return ImGui::IsKeyPressed(a,repeat)||(b!=ImGuiKey_None&&ImGui::IsKeyPressed(b,repeat));}
void border145(Canvas145 &c){
 c.fill(60,65,640,350,IM_COL32(12,12,13,245));
 c.dl->AddRect(c.p(60,65),c.p(700,415),IM_COL32(134,124,98,255),0,0,1.5f*c.scale);
 c.dl->PushClipRect(c.p(62,67),c.p(698,413),true);
 for(int x=50;x<710;x+=12){c.line((float)x,67,(float)x-20,87,IM_COL32(45,42,35,255),3);c.line((float)x,395,(float)x-18,413,IM_COL32(45,42,35,255),3);}
 c.dl->PopClipRect();
 for(int corner=0;corner<4;corner++){float x=corner&1?700:60,y=corner&2?415:65,dx=corner&1?-12:12,dy=corner&2?-12:12;c.line(x,y,x+dx,y,CREAM145,2);c.line(x,y,x,y+dy,CREAM145,2);}
}
}
extern "C" void graphics145_ready(void){hud145=recomp_settings_get("graphics","hud145",-1);position145=recomp_settings_get("graphics","hud_position145",0);hudStyle=stored145(graphics145::HUD_STYLE);}
extern "C" void graphics145_request(void){request145=1;}
extern "C" void graphics148_editor(void){editor145=1;}
extern "C" void graphics148_closed(void){active148=0;closed148=1;reopen148=clock148()+750;request145=0;}
extern "C" void graphics147_request_game(void){if(!active148.load()&&clock148()>=reopen148.load()){int expected=0;request145.compare_exchange_strong(expected,2);}}
extern "C" int graphics145_take_request(void){return request145.exchange(0);}
extern "C" int graphics145_android_state(int legacy_hud,int language){
 legacy_hud145=legacy_hud!=0;language145=std::max(0,std::min(2,language));
 int hud=hud145.load();if(hud<0)hud=legacy_hud;
 int pos=position145.load();pos=std::max(0,std::min(2,pos));
 return (hud?1:0)|(pos<<1)|(hudStyle.load()==1?8:0)|(editor145.exchange(0)?256:0)|(closed148.exchange(0)?512:0)|(framegen29.exchange(0)?1024:0)|(int)(restart148.exchange(0)<<12);
}
namespace graphics145 {
void open(){active148=1;leaving148=0;armed148=-1;wait_input147=false;from_game147=false;read145();page145=-1;selected145=0;leave145=0;confirm145=false;status145.clear();}
void open_game147(){open();from_game147=true;wait_input147=true;page145=0;}
bool draw(ImVec2 display){
 if(wait_input147&&armed148<0)armed148=ImGui::GetTime()+0.25;
 if(leave145&&!leaving148)leaving148=ImGui::GetTime()+0.25;
 if(leave145&&(ImGui::GetTime()>=leaving148||(!ImGui::IsMouseDown(0)&&!ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown)&&!ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight)&&!ImGui::IsKeyDown(ImGuiKey_Enter)&&!ImGui::IsKeyDown(ImGuiKey_Escape)&&!ImGui::IsKeyDown(ImGuiKey_K)&&!ImGui::IsKeyDown(ImGuiKey_L)))){
  fprintf(stderr,"[GRAPHICS148] closing; release_timeout=%d\n",ImGui::GetTime()>=leaving148);
  active148=0;closed148=1;reopen148=clock148()+750;request145=0;
  if(leave145==2)editor145=1;if(leave145==3){framegen29=1;fprintf(stderr,"[FRAMEGEN29] open_settings\n");}leave145=0;return false;
 }
 Canvas145 c(display);c.dl->AddRectFilled(ImVec2(0,0),display,IM_COL32(0,0,0,165));border145(c);
 c.text(word("OPTIONS","OPÇÕES","OPCIONES"),64,26,30,CREAM145,0);
 c.text("DEF JAM",696,38,14,GOLD145,1,true);
 bool back=key145(ImGuiKey_Escape,ImGuiKey_GamepadFaceRight)||key145(ImGuiKey_L);
 bool accept=key145(ImGuiKey_Enter,ImGuiKey_GamepadFaceDown)||key145(ImGuiKey_K);
 bool click=ImGui::IsMouseClicked(0);
 bool up=key145(ImGuiKey_UpArrow,ImGuiKey_GamepadDpadUp,true)||key145(ImGuiKey_GamepadLStickUp,ImGuiKey_None,true);
 bool down=key145(ImGuiKey_DownArrow,ImGuiKey_GamepadDpadDown,true)||key145(ImGuiKey_GamepadLStickDown,ImGuiKey_None,true);
 bool left=key145(ImGuiKey_LeftArrow,ImGuiKey_GamepadDpadLeft,true)||key145(ImGuiKey_GamepadLStickLeft,ImGuiKey_None,true);
 bool right=key145(ImGuiKey_RightArrow,ImGuiKey_GamepadDpadRight,true)||key145(ImGuiKey_GamepadLStickRight,ImGuiKey_None,true);
 if(leave145){back=accept=click=up=down=left=right=false;}
 if(wait_input147){if(ImGui::GetTime()>=armed148||(!ImGui::IsMouseDown(0)&&!ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown)&&!ImGui::IsKeyDown(ImGuiKey_Enter)&&!ImGui::IsKeyDown(ImGuiKey_K)))wait_input147=false;else back=accept=click=up=down=left=right=false;}
 if(confirm145){
  c.fill(150,145,460,165,IM_COL32(22,20,18,255));c.text(word("Discard unsaved changes?","Descartar alterações não salvas?","¿Descartar cambios sin guardar?"),175,172,21,CREAM145);
  c.fill(175,239,180,38,RED145);c.fill(388,239,180,38,IM_COL32(55,49,36,255));
  c.text(word("A  DISCARD","A  DESCARTAR","A  DESCARTAR"),188,245,16,CREAM145);c.text(word("B  KEEP EDITING","B  CONTINUAR","B  CONTINUAR"),400,245,16,CREAM145);
  if(accept||(click&&c.hit(175,239,180,38))){draft145.discard();confirm145=false;if(from_game147)leave145=1;else page145=-1;selected145=0;}
  if(back||(click&&c.hit(388,239,180,38)))confirm145=false;
  return true;
 }
 if(page145<0){
  c.text(word("GAME OPTIONS","OPÇÕES DO JOGO","OPCIONES DEL JUEGO"),88,98,17,GOLD145);
  const char *labels[]={word("GRAPHICS","GRÁFICOS","GRÁFICOS"),word("EDIT TOUCH CONTROLS","EDITAR CONTROLES","EDITAR CONTROLES"),word("FRAME GENERATION","GERAÇÃO DE QUADROS","GENERACIÓN DE CUADROS"),word("BACK TO GAME","VOLTAR AO JOGO","VOLVER AL JUEGO")};
  if(up)selected145=(selected145+3)%4;if(down)selected145=(selected145+1)%4;
  for(int i=0;i<4;i++){float y=132.f+i*56;
   if(i==selected145){c.fill(83,y-3,594,48,IM_COL32(56,20,22,220));c.dl->AddRect(c.p(83,y-3),c.p(677,y+45),RED145,0,0,3*c.scale);}
   c.text(labels[i],107,y+5,24,i==selected145?CREAM145:GREY145);
   if(click&&c.hit(83,y-3,594,48)){selected145=i;accept=true;}
  }
  if(back){leave145=1;return true;}
  if(accept){if(selected145==0){read145();page145=0;selected145=0;status145.clear();}else if(selected145==1){leave145=2;return true;}else if(selected145==2){leave145=3;return true;}else{leave145=1;return true;}}
  c.text(word("A  SELECT     B  BACK","A  SELECIONAR     B  VOLTAR","A  ELEGIR     B  ATRÁS"),88,437,16,CREAM145);
  return true;
 }
 c.text(word("GRAPHICS","GRÁFICOS","GRÁFICOS"),88,94,22,CREAM145,0);
 c.text("LB / RB  -  L1 / R1",88,120,9,GOLD145);
 const char *pages[]={word("DISPLAY","TELA","PANTALLA"),word("IMAGE","IMAGEM","IMAGEN"),word("PERF.","DESEMP.","REND."),word("SCALE / FG","ESCALA / FG","ESCALA / FG")};
 bool prev=key145(ImGuiKey_PageUp,ImGuiKey_GamepadL1)||key145(ImGuiKey_Q),next=key145(ImGuiKey_PageDown,ImGuiKey_GamepadR1)||key145(ImGuiKey_E);
 for(int i=0;i<4;i++){float x=327.f+i*90;c.text(pages[i],x,100,11,page145==i?GOLD145:GREY145);if(page145==i)c.line(x,120,x+78,120,RED145,3);if(click&&c.hit(x-4,93,88,32)){page145=i;selected145=0;click=false;}}
 if(prev||next){page145=(page145+(prev?3:1))%4;selected145=0;}
 int ids[COUNT],count=0;
 for(int i=0;i<COUNT;i++)if(i!=HUD_STYLE&&rows()[i].page==page145){
  ids[count++]=i;
  if(i==HUD_POSITION)ids[count++]=HUD_STYLE;
 }
 selected145=std::max(0,std::min(count-1,selected145));if(up)selected145=(selected145+count-1)%count;if(down)selected145=(selected145+1)%count;
 if(left||right)draft145.step(ids[selected145],left?-1:1);
 for(int at=0;at<count;at++){
  int id=ids[at];float y=134.f+at*28;bool selected=at==selected145;
  if(selected){c.fill(82,y-1,596,27,IM_COL32(73,24,25,235));c.line(82,y-1,678,y-1,RED145,2.5f);c.line(82,y+26,678,y+26,RED145,2.5f);}
  c.text(rows()[id].label[language145.load()],98,y+3,16,selected?CREAM145:GREY145);
  std::string val=value145(id)+(rows()[id].restart?" *":"");c.text(val.c_str(),646,y+3,16,selected?CREAM145:GREY145,1,true);
  if(selected){c.text("<",432,y+2,18,GOLD145);c.text(">",659,y+2,18,GOLD145);}
  if(click&&c.hit(82,y-1,596,27)){selected145=at;draft145.step(id,c.hit(418,y-1,42,27)?-1:1);status145.clear();}
 }
 const char *help=status145.empty()?help145(ids[selected145]):status145.c_str();
 ImFont *font=online_screen::menu_font145(2);if(!font)font=ImGui::GetFont();
 c.dl->AddText(font,13*c.scale,c.p(96,365),GOLD145,help,nullptr,560*c.scale);
 c.fill(84,428,181,32,IM_COL32(49,43,30,220));c.fill(286,428,174,32,IM_COL32(34,30,27,220));c.fill(482,428,196,32,IM_COL32(34,30,27,220));
 c.text(word("A  APPLY","A  APLICAR","A  APLICAR"),99,435,16,CREAM145);c.text(word("B  BACK","B  VOLTAR","B  ATRÁS"),303,435,16,CREAM145);c.text(word("Y  DEFAULTS","Y  RESTAURAR","Y  RESTAURAR"),497,435,16,CREAM145);
 if(accept||(click&&c.hit(84,428,181,32)))apply145();
 if(key145(ImGuiKey_GamepadFaceUp,ImGuiKey_I)||(click&&c.hit(482,428,196,32))){draft145.defaults();status145.clear();}
 if(back||(click&&c.hit(286,428,174,32))){if(draft145.dirty())confirm145=true;else{leave145=1;selected145=0;}}
 return true;
}
}
#endif
