#ifndef DEFJAM_GRAPHICS_MODEL145_H
#define DEFJAM_GRAPHICS_MODEL145_H
#include <array>
namespace graphics145 {
enum { SCALE,ASPECT,FPS,VSYNC,BILINEAR,HUD,HUD_POSITION,FXAA,FSR,SHARPNESS,FILTER,ANISO,GAMMA,VIGNETTE,PRESET,PRELOAD,BARRIERS,TEXTURES,SPATIAL,FG_ENABLE,FG_MULTIPLIER,FG_REFRESH,FG_FLOW,STORY_SUBTITLES,COUNT };
struct Row {const char *section,*key;const int *choices;int count,def,page;bool restart;const char *label[3];};
inline const Row *rows(){
 static const int boolean[]={0,1},scales[]={480,540,720,960,1080,1440,1920,2160},aspects[]={0,1,2,3,5},fps[]={-1,30,60,0},positions[]={0,1,2},bilinear[]={1,0},sharp[]={0,40,80,120,160,200},filters[]={0,1,2,3,4,5},presets[]={0,1,2,3},aniso[]={1,2,4,8,16},spatial[]={0,1,2},engines[]={0,1,2},mult[]={2,3,4},refresh[]={60,90,120},flow[]={25,50,100};
 static const Row r[]={
 {"display","render_height151",scales,8,480,0,true,{"Render resolution","Resolução interna","Resolución interna"}},
 {"display","aspect",aspects,5,2,0,true,{"Aspect ratio","Formato da tela","Formato de pantalla"}},
 {"vksched","present_limit",fps,4,-1,0,false,{"Frame limit","Limite de quadros","Límite de cuadros"}},
 {"display","vsync",boolean,2,0,0,true,{"Vertical sync","Sincronização vertical","Sincronización vertical"}},
 {"display","filter",bilinear,2,0,3,false,{"Bilinear filtering","Filtro bilinear","Filtro bilineal"}},
 {"graphics","hud145",boolean,2,1,0,false,{"FPS counter","Contador de FPS","Contador de FPS"}},
 {"graphics","hud_position145",positions,3,0,0,false,{"Counter position","Posição do contador","Posición del contador"}},
 {"graphics","fxaa145",boolean,2,0,1,false,{"Anti-aliasing (FXAA)","Suavização (FXAA)","Suavizado (FXAA)"}},
 {"graphics","fsr144",boolean,2,0,3,false,{"AMD FSR 1","AMD FSR 1","AMD FSR 1"}},
 {"graphics","fsr_sharpness144",sharp,6,80,3,false,{"FSR sharpness","Nitidez do FSR","Nitidez de FSR"}},
 {"graphics","screen_filter144",filters,6,0,1,false,{"Screen filter","Filtro de imagem","Filtro de imagen"}},
 {"graphics","anisotropy144",aniso,5,1,1,true,{"Anisotropic filtering","Filtro anisotrópico","Filtro anisotrópico"}},
 {"display","gamma",boolean,2,1,1,false,{"Game brightness curve","Curva de brilho do jogo","Curva de brillo del juego"}},
 {"display","vignette",boolean,2,1,1,false,{"Vignette","Vinheta","Viñeta"}},
 {"graphics","preset145",presets,4,0,2,false,{"Graphics preset","Perfil gráfico","Perfil gráfico"}},
 {"graphics","shader_preload144",boolean,2,1,2,true,{"Preload shaders","Pré-carregar shaders","Precargar shaders"}},
 {"graphics","precise_barriers144",boolean,2,1,2,true,{"Optimized Vulkan sync","Sincronização Vulkan otimizada","Sincronización Vulkan optimizada"}},
 {"textures","enabled",boolean,2,0,2,true,{"Texture packs / mods","Pacotes de textura / mods","Paquetes de texturas / mods"}},
 {"graphics","spatial34",spatial,3,0,3,true,{"Spatial upscaler","Ampliação espacial","Ampliación espacial"}},
 {"graphics","fg_engine39",engines,3,0,3,true,{"Frame generation engine","Motor de geração de quadros","Motor de generación de cuadros"}},
 {"graphics","lsfg30_multiplier",mult,3,2,3,true,{"FG multiplier","Multiplicador FG","Multiplicador FG"}},
 {"graphics","lsfg30_refresh",refresh,3,60,3,true,{"Output target","Alvo de saída","Salida objetivo"}},
 {"graphics","lsfg27_flow",flow,3,25,3,true,{"Optical-flow scale","Escala do fluxo óptico","Escala del flujo óptico"}},
 {"language","story_subtitles",engines,3,1,0,true,{"Story subtitles","Legendas da História","Subtítulos de Historia"}}
 };return r;
}
inline int sanitized(int id,int value){if(id==FPS&&value==45)return 30;const Row &r=rows()[id];for(int i=0;i<r.count;i++)if(r.choices[i]==value)return value;return r.def;}
struct Draft {
 std::array<int,COUNT> value{},saved{};
 void load(int id,int v){value[id]=saved[id]=sanitized(id,v);}
 void preset(int v){
  if(v==0)return;
  value[SCALE]=v==3?960:480;value[FPS]=-1;value[VSYNC]=v==3?1:0;
  value[BILINEAR]=v==1?1:0;value[FXAA]=v==3?1:0;value[FSR]=0;
  value[FILTER]=0;value[ANISO]=v==3?4:1;value[GAMMA]=v==1?0:1;
  value[VIGNETTE]=v==1?0:1;value[TEXTURES]=0;value[PRELOAD]=1;value[BARRIERS]=1;
 }
 void step(int id,int direction){const Row &r=rows()[id];int at=0;for(int i=0;i<r.count;i++)if(r.choices[i]==value[id])at=i;at=(at+(direction<0?-1:1)+r.count)%r.count;value[id]=r.choices[at];if(id==PRESET)preset(value[id]);else value[PRESET]=0;}

 bool dirty()const{return value!=saved;}
 bool needs_restart()const{for(int i=0;i<COUNT;i++)if(rows()[i].restart&&value[i]!=saved[i])return true;return false;}
 void defaults(){for(int i=0;i<COUNT;i++)value[i]=rows()[i].def;}
 void discard(){value=saved;}
 void accepted(){saved=value;}
};
}
#endif
