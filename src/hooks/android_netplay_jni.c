#if defined(__ANDROID__)
#include <jni.h>
#include "android_netplay.h"
#include "automatch.h"
#include "netplay.h"
JNIEXPORT jint JNICALL Java_org_defjamffny_android_OnlinePanel_nativeMenuEvent(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;return android_netplay_menu_status();
}
JNIEXPORT jboolean JNICALL Java_org_defjamffny_android_OnlinePanel_nativeFailed(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;return netplay_state()==NETPLAY_FAILED?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT jboolean JNICALL Java_org_defjamffny_android_OnlinePanel_nativeCanOpen(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;return automatch_android_main_menu()?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT jint JNICALL Java_org_defjamffny_android_OnlinePanel_nativeCommand(
        JNIEnv *env,jclass cls,jint action,jstring text,jstring extra,jint value) {
    (void)cls;
    const char *a=text?(*env)->GetStringUTFChars(env,text,NULL):NULL;
    if (text&&!a) return -1;
    const char *b=extra?(*env)->GetStringUTFChars(env,extra,NULL):NULL;
    if (extra&&!b) { if(a)(*env)->ReleaseStringUTFChars(env,text,a);return -1; }
    int result=android_netplay_command(action,a?a:"",b?b:"",value);
    if(a)(*env)->ReleaseStringUTFChars(env,text,a);
    if(b)(*env)->ReleaseStringUTFChars(env,extra,b);
    return result;
}
JNIEXPORT jstring JNICALL Java_org_defjamffny_android_OnlinePanel_nativeSnapshot(JNIEnv *env,jclass cls) {
    (void)cls;char json[32768];
    if (!android_netplay_snapshot(json,sizeof json)) return (*env)->NewStringUTF(env,"{}");
    return (*env)->NewStringUTF(env,json);
}
JNIEXPORT void JNICALL Java_org_defjamffny_android_OnlinePanel_nativeAbort(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;defjam_android_online_cancel();android_netplay_abort();
}
#endif
