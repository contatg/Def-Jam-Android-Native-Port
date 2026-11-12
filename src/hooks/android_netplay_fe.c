/* Guest calls stay on the guest thread. Android never patches the disc's menus. */
#if defined(__ANDROID__)
#include "guest_call.h"
#include "netplay.h"
#include "automatch.h"
#include <string.h>
extern void sub_00068BA0(void);
extern void sub_000D6670(void);
unsigned int defjam_fe_call(const char *command) {
    GuestRegs r; guest_regs_save(&r);
    double fp[8];memcpy(fp,g_fp_stack,sizeof fp);
    uint16_t cw=g_fp_control_word,cc=g_fp_cc;int cmp=g_fp_cmp,df=g_df;
    RecompXmm xmm[8]={g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7};
#ifdef RECOMP_MMX_DEFINED
    RecompMmx mm[8]={g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7};
#endif
    size_t n=strlen(command)+1;
    if(n>512||g_esp<4096) return 0;
    uint32_t text=(g_esp-0x40u-(uint32_t)n)&~3u;
    for(size_t i=0;i<n;i++)MEM8(text+(uint32_t)i)=(uint8_t)command[i];
    g_esp=text;PUSH32(g_esp,text);PUSH32(g_esp,0);
    sub_00068BA0();uint32_t result=g_eax;
    guest_regs_restore(&r);memcpy(g_fp_stack,fp,sizeof fp);
    g_fp_control_word=cw;g_fp_cc=cc;g_fp_cmp=cmp;g_df=df;
    g_xmm0=xmm[0];g_xmm1=xmm[1];g_xmm2=xmm[2];g_xmm3=xmm[3];
    g_xmm4=xmm[4];g_xmm5=xmm[5];g_xmm6=xmm[6];g_xmm7=xmm[7];
#ifdef RECOMP_MMX_DEFINED
    g_mm0=mm[0];g_mm1=mm[1];g_mm2=mm[2];g_mm3=mm[3];
    g_mm4=mm[4];g_mm5=mm[5];g_mm6=mm[6];g_mm7=mm[7];
#endif
    return result;
}
void android_netplay_help(void) {
    if(netplay_active()){g_esp+=4;return;}
    sub_000D6670();
}
#endif
