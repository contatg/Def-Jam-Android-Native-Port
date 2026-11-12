#if defined(__ANDROID__)
/* The online hub's lists. See online_data.h. */
#define _CRT_SECURE_NO_WARNINGS
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "online_data.h"
#if defined(_WIN32)
#include <windows.h>
#elif defined(__ANDROID__)
#include "host.h"
#include <dirent.h>
#include <strings.h>
#endif

#define MAX_FIGHTERS 160
#define MAX_VENUES   48

static const OnlineMode k_modes[] = {
    { 0, "One on One", 2 },
    { 2, "3-Way", 3 },
    { 3, "Free for All", 4 },
};

static OnlineFighter g_fighters[MAX_FIGHTERS];
static OnlineVenue g_venues[MAX_VENUES];
static int g_fighter_count, g_venue_count;
#if !defined(_WIN32) && !defined(__ANDROID__)
static int g_loaded;
#endif

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
#if defined(__ANDROID__)
    if(!f&&!strncmp(path,"game/",5)&&!strchr(path+5,'/')){
        /* Xbox disc names ignore case. The roster bypasses the kernel path resolver. */
        DIR *dir=opendir("game");struct dirent *entry;char resolved[1024]={0};int matches=0;
        if(dir){while((entry=readdir(dir)))if(!strcasecmp(entry->d_name,path+5)){
            if(++matches==1)snprintf(resolved,sizeof resolved,"game/%s",entry->d_name);
        }closedir(dir);}
        if(matches==1){f=fopen(resolved,"rb");if(f)fprintf(stderr,"[ONLINE-DATA126] case-resolved %s\n",path);}
    }
#endif
    long size;
    char *text;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (8 << 20)) { fclose(f); return NULL; }
    text = (char *)malloc((size_t)size + 1);
    if (text && fread(text, 1, (size_t)size, f) != (size_t)size) { free(text); text = NULL; }
    if (text) text[size] = 0;
    fclose(f);
    return text;
}

/* The text of <tag>...</tag> inside [from, to), trimmed, into out. 0 if absent. */
static int field(const char *from, const char *to, const char *tag, char *out, int size)
{
    char open[32];
    const char *a, *b;
    int n;
    snprintf(open, sizeof(open), "<%s>", tag);
    a = strstr(from, open);
    if (!a || a >= to) return 0;
    a += strlen(open);
    b = strchr(a, '<');
    if (!b || b > to) return 0;
    while (a < b && isspace((unsigned char)*a)) a++;
    while (b > a && isspace((unsigned char)b[-1])) b--;
    n = (int)(b - a) < size - 1 ? (int)(b - a) : size - 1;
    memcpy(out, a, (size_t)n);
    out[n] = 0;
    return 1;
}

static const char *style_name(const char *code)
{
    if (!strcmp(code, "STF")) return "Street fighting";
    if (!strcmp(code, "MAR")) return "Martial arts";
    if (!strcmp(code, "KIC")) return "Kickboxing";
    if (!strcmp(code, "WRE")) return "Wrestling";
    if (!strcmp(code, "SUB")) return "Submissions";
    return NULL;
}

/* "Doc (MAR)" -> name "Doc", style "Martial arts". Entries without a known style are not
 * selectable fighters (placeholders, story figures). */
static void load_fighters(void)
{
    char *text = read_file("game/fighter.xml"), *p;
    if (!text) return;
    for (p = strstr(text, "<fighter>"); p && g_fighter_count < MAX_FIGHTERS; p = strstr(p + 1, "<fighter>")) {
        char *end = strstr(p, "</fighter>"), disp[64], id[16], *paren;
        const char *style;
        OnlineFighter *f;
        if (!end) break;
        if (!field(p, end, "dispname", disp, sizeof(disp)) || !field(p, end, "ID", id, sizeof(id))) continue;
        paren = strrchr(disp, '(');
        if (!paren || strlen(paren) != 5 || paren[4] != ')') {
            /* Lauren is a Battle-mode fighter whose entry has no style (players asked for her) */
            if (strcmp(disp, "Lauren")) continue;
            style = "";
        } else {
            paren[4] = 0;
            style = style_name(paren + 1);
            if (!style) continue;
            while (paren > disp && paren[-1] == ' ') paren--;
            *paren = 0;
        }
        /* Teck, Santos, O.E and Eric Sermon: story figures without a select portrait. A match with
         * any of them crashes the load (WrestlerSpecInit, docs/research/crash-001B2480.md), also as
         * the CPU that fills a mode's free places, so they are in no list at all (the relay plays
         * them as Doc for older builds). */
        if (atoi(id) == 40 || atoi(id) == 41 || atoi(id) == 52 || atoi(id) == 78) continue;
        f = &g_fighters[g_fighter_count++];
        f->id = atoi(id);
        snprintf(f->name, sizeof(f->name), "%s", disp);
        f->style = style;
    }
    free(text);
}

/* The arenas: a "NNx Name" display name with a front-end ID, not cut, not cinema only. */
static void load_venues(void)
{
    char *text = read_file("game/venue.xml"), *p;
    if (!text) return;
    for (p = strstr(text, "<venue>"); p && g_venue_count < MAX_VENUES; p = strstr(p + 1, "<venue>")) {
        char *end = strstr(p, "</venue>"), disp[80], fe[16], *name, *paren;
        int fe_id, i, dup = 0;
        if (!end) break;
        if (!field(p, end, "dispname", disp, sizeof(disp)) || !field(p, end, "feID", fe, sizeof(fe))) continue;
        fe_id = atoi(fe);
        if (fe_id <= 0 || fe_id > 255) continue;
        if (!(isdigit((unsigned char)disp[0]) && isdigit((unsigned char)disp[1]) && isalpha((unsigned char)disp[2]) && disp[3] == ' '))
            continue;
        if (strstr(disp, "CUT") || strstr(disp, "Cinema")) continue;
        for (i = 0; i < g_venue_count; i++) if (g_venues[i].fe_id == fe_id) dup = 1;
        if (dup) continue;
        name = disp + 4;
        paren = strchr(name, '(');
        if (paren) {
            while (paren > name && paren[-1] == ' ') paren--;
            *paren = 0;
        }
        g_venues[g_venue_count].fe_id = fe_id;
        snprintf(g_venues[g_venue_count].name, sizeof(g_venues[0].name), "%s", name);
        g_venue_count++;
    }
    free(text);
}

#if defined(_WIN32) || defined(__ANDROID__)
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK load_once(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    load_fighters();
    load_venues();
#if defined(__ANDROID__)
    fprintf(stderr,"[ONLINE-DATA126] roster=%d venues=%d; read from this installation's game XML\n",g_fighter_count,g_venue_count);
#endif
    return TRUE;
}
static void load(void) { InitOnceExecuteOnce(&g_once, load_once, NULL, NULL); }
#else
static void load(void)
{
    if (g_loaded) return;
    g_loaded = 1;
    load_fighters();
    load_venues();
}
#endif

int online_fighters(const OnlineFighter **out) { load(); *out = g_fighters; return g_fighter_count; }
int online_venues(const OnlineVenue **out) { load(); *out = g_venues; return g_venue_count; }
int online_modes(const OnlineMode **out) { *out = k_modes; return (int)(sizeof(k_modes) / sizeof(k_modes[0])); }

const char *online_fighter_name(int id)
{
    static char unknown[4][24];
    static int next;
    int i;
    load();
    for (i = 0; i < g_fighter_count; i++) if (g_fighters[i].id == id) return g_fighters[i].name;
    next = (next + 1) % 4;
    snprintf(unknown[next], sizeof(unknown[0]), "Fighter %d", id);
    return unknown[next];
}

const char *online_venue_name(int fe_id)
{
    static char unknown[4][24];
    static int next;
    int i;
    load();
    for (i = 0; i < g_venue_count; i++) if (g_venues[i].fe_id == fe_id) return g_venues[i].name;
    next = (next + 1) % 4;
    snprintf(unknown[next], sizeof(unknown[0]), "Venue %d", fe_id);
    return unknown[next];
}

const OnlineMode *online_mode(int type)
{
    unsigned i;
    for (i = 0; i < sizeof(k_modes) / sizeof(k_modes[0]); i++) if (k_modes[i].type == type) return &k_modes[i];
    return NULL;
}


#endif
