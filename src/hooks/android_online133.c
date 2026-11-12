/* Android account/voice adapters for sweeax-exe/ffny-online (MIT).
 * FFNY-ONLINE-LICENSE.txt applies. No Discord credentials or bearer stored in C.
 */
#if defined(__ANDROID__)
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "account.h"

extern uint32_t netplay_voice_context133(void);
extern void netplay_voice_send133(uint32_t room,const uint8_t *payload,int len);
static JavaVM *s_vm;
static jclass s_account_class;
static jmethodID s_ranked,s_report,s_fetch_stats,s_voice_settings;
static char s_stats[16384];static int s_stats_busy;
static pthread_mutex_t s_account_lock=PTHREAD_MUTEX_INITIALIZER;
static int s_account_state;
static char s_name[25],s_reason[256];

JNIEXPORT void JNICALL Java_org_defjamffny_android_OnlineAccount133_nativeBind(JNIEnv *e,jclass c)
{
    if(!s_vm){fprintf(stderr,"[ACCOUNT133] Android account bridge ready; no boot-time HTTP\n");(*e)->GetJavaVM(e,&s_vm);s_account_class=(*e)->NewGlobalRef(e,c);
        s_ranked=(*e)->GetStaticMethodID(e,c,"rankedTicket","()Ljava/lang/String;");
        s_report=(*e)->GetStaticMethodID(e,c,"reportMatch","(Ljava/lang/String;)V");
        s_fetch_stats=(*e)->GetStaticMethodID(e,c,"fetchStats","(I)V");
        s_voice_settings=(*e)->GetStaticMethodID(e,c,"openVoiceSettings","()V");}
}
JNIEXPORT void JNICALL Java_org_defjamffny_android_OnlineAccount133_nativeState(JNIEnv *e,jclass c,jint state,jstring name,jstring reason)
{
    (void)c;const char*n=name?(*e)->GetStringUTFChars(e,name,0):NULL;
    const char*r=reason?(*e)->GetStringUTFChars(e,reason,0):NULL;
    pthread_mutex_lock(&s_account_lock);s_account_state=state>=0&&state<=4?state:ACCOUNT_FAILED;
    if(s_account_state==ACCOUNT_OUT||s_account_state==ACCOUNT_FAILED)s_stats[0]=0;
    snprintf(s_name,sizeof s_name,"%s",n?n:"");snprintf(s_reason,sizeof s_reason,"%s",r?r:"");pthread_mutex_unlock(&s_account_lock);
    if(n)(*e)->ReleaseStringUTFChars(e,name,n);if(r)(*e)->ReleaseStringUTFChars(e,reason,r);
    fprintf(stderr,"[ACCOUNT133] state=%d; no credentials or account identity logged\n",(int)state);
}
enum AccountState account_state(void){pthread_mutex_lock(&s_account_lock);int s=s_account_state;pthread_mutex_unlock(&s_account_lock);return (enum AccountState)s;}
int account_game_name(char*out,int size){if(!out||size<1)return 0;pthread_mutex_lock(&s_account_lock);int ok=s_account_state==ACCOUNT_IN&&s_name[0];snprintf(out,(size_t)size,"%s",ok?s_name:"");pthread_mutex_unlock(&s_account_lock);return ok;}
void account_text(char*out,int size){if(!out||size<1)return;pthread_mutex_lock(&s_account_lock);snprintf(out,(size_t)size,"%s",s_account_state==ACCOUNT_IN?s_name:s_reason);pthread_mutex_unlock(&s_account_lock);}
const char*account_privacy_url(void){return "https://play.defjam.online/privacy";}
static JNIEnv*account_env133(int*attached)
{
    JNIEnv*e=NULL;*attached=0;if(!s_vm||!s_account_class)return NULL;
    if((*s_vm)->GetEnv(s_vm,(void**)&e,JNI_VERSION_1_6)==JNI_OK)return e;
    if((*s_vm)->AttachCurrentThread(s_vm,(void*)&e,NULL)!=JNI_OK)return NULL;*attached=1;return e;
}
JNIEXPORT void JNICALL Java_org_defjamffny_android_OnlineAccount133_nativeStats(JNIEnv*e,jclass c,jboolean busy,jstring text)
{
    (void)c;const char*t=text?(*e)->GetStringUTFChars(e,text,NULL):NULL;
    pthread_mutex_lock(&s_account_lock);s_stats_busy=busy!=0;if(!busy)snprintf(s_stats,sizeof s_stats,"%s",t?t:"");pthread_mutex_unlock(&s_account_lock);
    if(t)(*e)->ReleaseStringUTFChars(e,text,t);
}
void account_fetch_stats_page(int page)
{
    int attached;JNIEnv*e=account_env133(&attached);if(!e||!s_fetch_stats)return;
    (*e)->CallStaticVoidMethod(e,s_account_class,s_fetch_stats,(jint)page);
    if((*e)->ExceptionCheck(e))(*e)->ExceptionClear(e);if(attached)(*s_vm)->DetachCurrentThread(s_vm);
}
void account_fetch_stats(void){account_fetch_stats_page(0);}
int account_stats_busy(void){pthread_mutex_lock(&s_account_lock);int busy=s_stats_busy;pthread_mutex_unlock(&s_account_lock);return busy;}
int account_stats(char*out,int size){if(!out||size<1)return 0;pthread_mutex_lock(&s_account_lock);int ok=s_stats[0]!=0;snprintf(out,(size_t)size,"%s",s_stats);pthread_mutex_unlock(&s_account_lock);return ok;}
void android_voice_settings134(void)
{
    int attached;JNIEnv*e=account_env133(&attached);if(!e||!s_voice_settings)return;
    (*e)->CallStaticVoidMethod(e,s_account_class,s_voice_settings);
    if((*e)->ExceptionCheck(e))(*e)->ExceptionClear(e);if(attached)(*s_vm)->DetachCurrentThread(s_vm);
}
int account_ranked_ticket(char out[33])
{
    out[0]=0;if(account_state()!=ACCOUNT_IN||!s_ranked)return 0;int attached,ok=0;JNIEnv*e=account_env133(&attached);if(!e)return 0;
    jstring value=(jstring)(*e)->CallStaticObjectMethod(e,s_account_class,s_ranked);
    if((*e)->ExceptionCheck(e))(*e)->ExceptionClear(e);
    else if(value){const char*t=(*e)->GetStringUTFChars(e,value,NULL);if(t&&strlen(t)==32){ok=1;for(int i=0;i<32;i++)if(!((t[i]>='0'&&t[i]<='9')||(t[i]>='a'&&t[i]<='f')||(t[i]>='A'&&t[i]<='F')))ok=0;if(ok)memcpy(out,t,33);}if(t)(*e)->ReleaseStringUTFChars(e,value,t);}
    if(value)(*e)->DeleteLocalRef(e,value);if(attached)(*s_vm)->DetachCurrentThread(s_vm);return ok;
}
void account_report_match(const char*json)
{
    if(account_state()!=ACCOUNT_IN||!json||strlen(json)>16384||!s_report)return;
    int attached;JNIEnv*e=account_env133(&attached);if(!e)return;jstring j=(*e)->NewStringUTF(e,json);
    if(j){(*e)->CallStaticVoidMethod(e,s_account_class,s_report,j);(*e)->DeleteLocalRef(e,j);}
    if((*e)->ExceptionCheck(e))(*e)->ExceptionClear(e);if(attached)(*s_vm)->DetachCurrentThread(s_vm);
}

/* A bounded inbox: the relay thread only copies a small packet; no audio API,
 * decoder, Java callback, blocking network or heap allocation on that thread. */
#define VOICE_BYTES133 326
#define VOICE_QUEUE133 32
static pthread_mutex_t s_voice_lock=PTHREAD_MUTEX_INITIALIZER;
static struct{uint32_t room;int slot;uint8_t bytes[VOICE_BYTES133];}s_voice[VOICE_QUEUE133];
static unsigned s_voice_rd,s_voice_wr;static int s_voice_enabled;
void android_voice_packet133(int slot,const uint8_t*p,int len)
{
    if(slot<0||slot>=10||len!=VOICE_BYTES133||!p||p[0]!=7)return;
    uint32_t room=netplay_voice_context133();if(!room)return;
    pthread_mutex_lock(&s_voice_lock);
    if(s_voice_enabled){if(s_voice_wr-s_voice_rd>=VOICE_QUEUE133)s_voice_rd++;
        unsigned at=s_voice_wr++%VOICE_QUEUE133;s_voice[at].room=room;s_voice[at].slot=slot;memcpy(s_voice[at].bytes,p,VOICE_BYTES133);}
    pthread_mutex_unlock(&s_voice_lock);
}
/* Called only while netplay holds g_cs: keep the same lock order as receive.
 * A stop waits for an in-flight send, and no later send passes this gate. */
int android_voice_begin_send133(void)
{
    pthread_mutex_lock(&s_voice_lock);
    if(!s_voice_enabled){pthread_mutex_unlock(&s_voice_lock);return 0;}
    return 1;
}
void android_voice_end_send133(void){pthread_mutex_unlock(&s_voice_lock);}
JNIEXPORT void JNICALL Java_org_defjamffny_android_VoiceChat133_nativeEnabled(JNIEnv*e,jclass c,jboolean enabled)
{(void)e;(void)c;pthread_mutex_lock(&s_voice_lock);s_voice_enabled=enabled!=0;s_voice_rd=s_voice_wr=0;pthread_mutex_unlock(&s_voice_lock);}
JNIEXPORT jlong JNICALL Java_org_defjamffny_android_VoiceChat133_nativeRoom(JNIEnv*e,jclass c)
{(void)e;(void)c;return (jlong)netplay_voice_context133();}
JNIEXPORT jbyteArray JNICALL Java_org_defjamffny_android_VoiceChat133_nativePop(JNIEnv*e,jclass c,jlong room)
{
    (void)c;uint8_t p[VOICE_BYTES133+1];int got=0;
    pthread_mutex_lock(&s_voice_lock);
    while(s_voice_rd!=s_voice_wr){unsigned at=s_voice_rd++%VOICE_QUEUE133;if(s_voice[at].room!=(uint32_t)room)continue;
        p[0]=(uint8_t)s_voice[at].slot;memcpy(p+1,s_voice[at].bytes,VOICE_BYTES133);got=1;break;}
    pthread_mutex_unlock(&s_voice_lock);if(!got)return NULL;
    jbyteArray b=(*e)->NewByteArray(e,sizeof p);if(b)(*e)->SetByteArrayRegion(e,b,0,sizeof p,(const jbyte*)p);return b;
}
JNIEXPORT void JNICALL Java_org_defjamffny_android_VoiceChat133_nativeSend(JNIEnv*e,jclass c,jlong room,jbyteArray data)
{
    (void)c;if(!data||(*e)->GetArrayLength(e,data)!=VOICE_BYTES133)return;
    uint8_t p[VOICE_BYTES133];(*e)->GetByteArrayRegion(e,data,0,sizeof p,(jbyte*)p);
    /* The network lock must precede the inbox lock on both send and receive. */
    if(!(*e)->ExceptionCheck(e))netplay_voice_send133((uint32_t)room,p,sizeof p);
}
#endif
