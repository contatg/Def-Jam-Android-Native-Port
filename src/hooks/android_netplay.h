#ifndef DEFJAM_ANDROID_NETPLAY_H
#define DEFJAM_ANDROID_NETPLAY_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Called only by the serialized Android networking worker, never the UI/game thread. */
int android_netplay_command(int action, const char *text, const char *extra, int value);
int android_netplay_snapshot(char *out, size_t size);
/* Nonblocking: release guest waits before SDL pauses; worker cleanup follows. */
void android_netplay_abort(void);
int android_netplay_prepare_ports(int players);
/* Read-only timing around one unchanged online step; no protocol/input edits. */
void android_netplay_profile_step(void);
/* Original guest menu -> nonblocking Android UI request. */
void android_netplay_menu_event(const char *text);
int android_netplay_menu_status(void);
void android_netplay_menu_cancel(void);
void defjam_android_online_cancel(void);
#ifdef __cplusplus
}
#endif
#endif
