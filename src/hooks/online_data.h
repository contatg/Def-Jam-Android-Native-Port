/* What the online hub offers: the fighters, venues and match types, read at run time from the
 * player's own dump (game/fighter.xml, game/venue.xml), so no names ship with the port. */
#ifndef DEFJAM_ONLINE_DATA_H
#define DEFJAM_ONLINE_DATA_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OnlineFighter {
    int id;             /* fighter.xml ID: what Controller.ControllerSetup takes */
    char name[32];
    const char *style;  /* "Street fighting", ...; "" if none */
} OnlineFighter;

typedef struct OnlineVenue {
    int fe_id;          /* venue.xml feID: what Game.ChooseVenue takes */
    char name[48];
} OnlineVenue;

typedef struct OnlineMode {
    int type;           /* front-end match-type index (Game.SetMatchType) */
    const char *name;
    int fighters;       /* fighters in the match; free places go to the CPU */
} OnlineMode;

/* Each returns the count and sets *out to a list that lives as long as the process. The two
 * from the dump are empty when it cannot be read. */
int online_fighters(const OnlineFighter **out);
int online_venues(const OnlineVenue **out);
int online_modes(const OnlineMode **out);

const char *online_fighter_name(int id);    /* "Fighter 12" if unknown */
const char *online_venue_name(int fe_id);
const OnlineMode *online_mode(int type);    /* NULL if not offered */

#ifdef __cplusplus
}
#endif

#endif

