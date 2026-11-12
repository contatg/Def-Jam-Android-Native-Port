/* Original main-menu entry prepared from the player's own archive.
 * No game bytes live in this source or the APK. */
#if defined(__ANDROID__)
#include "android_netplay.h"
#include "automatch.h"
#include "../../tools/xboxrecomp/src/kernel/kernel.h"
#include <stdatomic.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static _Atomic int s_available, s_open;

void *defjam_android_menu_open(const char *path, uint32_t access,
                               uint32_t disposition, uint32_t options)
{
    const uint32_t writes = XBOX_GENERIC_WRITE | XBOX_GENERIC_ALL | XBOX_DELETE |
        XBOX_FILE_WRITE_DATA | XBOX_FILE_APPEND_DATA | XBOX_FILE_WRITE_ATTRIBUTES;
    if (!path || (access & writes) || disposition != XBOX_FILE_OPEN ||
        (options & XBOX_FILE_DIRECTORY_FILE)) return NULL;
    if (!strncasecmp(path,"\\??\\",4))path+=4;
    if (strcasecmp(path,"\\Device\\CdRom0\\screens\\screens.viv") &&
        strcasecmp(path,"D:\\screens\\screens.viv")) return NULL;
    const char *override = "override/screens/screens.viv";
    int fd = open(override, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;char tag[4];
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<16 ||
        pread(fd,tag,sizeof tag,0)!=sizeof tag ||
        (memcmp(tag,"BIGF",4)&&memcmp(tag,"BIG4",4))) {
        close(fd);fprintf(stderr,"[ONLINE-MENU] invalid override; using original archive\n");
        return NULL;
    }
    HANDLE handle = w32_open_handle(fd, override);
    if (!handle) {close(fd);return NULL;}
    if (!atomic_exchange(&s_available,1))
        fprintf(stderr,"[ONLINE-MENU] original menu ONLINE override active (local dump)\n");
    return handle;
}

void android_netplay_menu_event(const char *text)
{
    if (text && !strcmp(text,"Online.Open()") && automatch_android_main_menu())
        atomic_store(&s_open,1);
}

int android_netplay_menu_status(void)
{
    int open = atomic_exchange(&s_open,0);
    return atomic_load(&s_available) |
        ((open && automatch_android_main_menu()) ? 2 : 0);
}

void android_netplay_menu_cancel(void) {atomic_store(&s_open,0);}
#endif
