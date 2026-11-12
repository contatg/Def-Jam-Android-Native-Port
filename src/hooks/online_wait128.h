#ifndef DEFJAM_ONLINE_WAIT128_H
#define DEFJAM_ONLINE_WAIT128_H
#include <stdint.h>
/* Auto-reset input-arrived event wakes immediately. Timeout is only the next
 * resend deadline, not a mandatory sleep or a prediction of remote inputs. */
static unsigned online_wait128(uint64_t now,uint64_t last_send)
{
    if(now<last_send||now-last_send>=50)return 1;
    return (unsigned)(50-(now-last_send));
}
#endif
