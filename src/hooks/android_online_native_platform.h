/* Android platform adapter for the MIT PC screen; no network protocol changes. */
#ifndef DEFJAM_ANDROID_ONLINE_NATIVE_PLATFORM_H
#define DEFJAM_ANDROID_ONLINE_NATIVE_PLATFORM_H
#include "android_netplay.h"
#include "automatch.h"
#include "platform/win32_compat.h"
#include "platform/android_thread_policy.h"
#include "imgui_impl_vulkan.h"
#include <mutex>
#include <condition_variable>
#include <thread>
#include <jni.h>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "../../tools/xboxrecomp/src/nv2a/stb/stb_image.h"

extern "C" void xbox_UsbSetPadCount(int);
extern "C" void online_menu_on_funccall(const char *);
namespace android_online {
static std::mutex mutex;
static std::condition_variable wake;
static std::thread worker;
static bool started,stop,pending,cleanup,busy,owns_session;
static std::atomic<bool> hide_requested{false};
static std::string server="",name="Player",queued_server,queued_name;
static void run(){
    defjam_android_native_thread_enter("online","native_online_commands",(uintptr_t)run);
    std::unique_lock<std::mutex> lock(mutex);
    for(;;){
        wake.wait(lock,[]{return stop||pending||cleanup;});
        if(stop)break;
        bool close=cleanup;cleanup=false;
        std::string host=queued_server,player=queued_name;bool connect=pending;pending=false;busy=true;
        lock.unlock();
        if(close)netplay_stop();
        if(connect)android_netplay_command(0,host.c_str(),player.c_str(),0);
        lock.lock();
        busy=false;
        if(close&&!pending)owns_session=false;
    }
    lock.unlock();netplay_stop();
}
static void cancel(){
    std::lock_guard<std::mutex> lock(mutex);
    pending=false;hide_requested=true;
    if(owns_session){cleanup=true;android_netplay_abort();wake.notify_one();}
}
static void shutdown(){
    {std::lock_guard<std::mutex> lock(mutex);stop=true;pending=false;android_netplay_abort();wake.notify_one();}
    if(worker.joinable())worker.join();
}
static int connect(const char *,const char *){
    std::lock_guard<std::mutex> lock(mutex);
    // A cancel finishes on this worker before a new connection can start.
    // Repeated taps must not reset a live/connecting relay from another thread.
    if(server.empty())return -1;
    if(stop||pending||cleanup||busy)return 0;
    if(!started){worker=std::thread(run);started=true;atexit(shutdown);}
    queued_server=server;queued_name=name;pending=true;owns_session=true;wake.notify_one();return 0;
}
static void ready(int ready,int fighter){
    NetRoom room;netplay_lobby_room(&room);
    if(room.in_room&&room.phase==0&&automatch_android_main_menu()){
        xbox_UsbSetPadCount(room.settings.max);netplay_lobby_ready(ready,fighter);
    }
}
// Account statistics now use the Android HTTPS/JNI adapter. The unused PC voice
// page stays isolated; Android opens its actual host voice settings instead.
static void account_noop(){}
static int account_empty(char *out,int size){if(out&&size>0)out[0]=0;return 0;}
static void account_page(int){}
static int account_idle(){return 0;}
static float mic_none(){return -1;}
static int player_volume(int){return 100;}
static void set_volume(int,int){}
}
#define netplay_lobby android_online::connect
#define netplay_lobby_ready android_online::ready
#define account_init android_online::account_noop
#define account_refresh android_online::account_noop




#define voice_mic_meter android_online::mic_none
#define voice_player_volume android_online::player_volume
#define voice_set_player_volume android_online::set_volume
#endif

extern "C" void android_voice_settings134(void);
