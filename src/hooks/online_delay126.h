#ifndef DEFJAM_ONLINE_DELAY126_H
#define DEFJAM_ONLINE_DELAY126_H
/* Additive P_RTT capability. Older rules4 clients ignore bytes8/9 and keep10.
 * No input prediction, RNG/step change or unilateral mid-match delay change. */
static int online_delay126(const int *rtt, const int *rollback, const int *capable,
                           int players, int *required, int *extended)
{
    int a=0,b=0,all=1,rb=1;
    if(players<2||players>4)return -1;
    for(int i=0;i<players;i++){
        if(rtt[i]<0)return -1; /* never guess a different delay from missing pings */
        int r=rtt[i]>2000?2000:rtt[i];
        if(r>a){b=a;a=r;}else if(r>b)b=r;
        if(capable[i]!=1)all=0;
        if(!rollback[i])rb=0;
    }
    int d=(((a+b)/2)*3+49)/50+1; /* exact legacy rounding, 60Hz */
    if(rb)d-=4;
    if(required)*required=d;
    if(extended)*extended=all;
    int cap=all?32:10;
    return d<2?2:d>cap?cap:d;
}
#endif
