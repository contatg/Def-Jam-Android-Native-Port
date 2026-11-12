/* Front-end command injection (RECOMP_FE_INJECT). See fe_inject.c. */

#ifndef DEFJAM_FE_INJECT_H
#define DEFJAM_FE_INJECT_H

typedef struct FeInjectRule {
    char anchor[96];        /* lower case */
    int want, seen, fired;
    char commands[384];     /* separated by ';' */
} FeInjectRule;

/* Rules from "anchor[#N]=>cmd[;cmd]|...". Returns how many were read. */
int fe_inject_parse(const char *spec, FeInjectRule *rules, int max);

/* Called by the FuncCall hook after each front-end call has run. */
void fe_inject_after_call(const char *text);

#endif
