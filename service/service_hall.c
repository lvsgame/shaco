#include "sc_service.h"
#include "sc_node.h"
#include "sh_hash.h"
#include "hall.h"
#include "player.h"
#include "playerdb.h"
#include "rolelogic.h"
#include "ringlogic.h"
#include "awardlogic.h"
#include "playlogic.h"
#include "user_message.h"
#include "cli_message.h"
#include <stdlib.h>

struct hall *
hall_create() {
    struct hall *self = malloc(sizeof(*self));
    memset(self, 0, sizeof(*self));
    return self;
}

void
hall_free(struct hall *self) {
    if (self == NULL)
        return;
    player_fini(self);
    playerdb_fini(self); 
    free(self);
}

int
hall_init(struct service *s) {
    struct hall *self = SERVICE_SELF;

    if (sh_handler("watchdog", &self->watchdog_handle) ||
        sh_handler("match", &self->match_handle) ||
        sh_handler("rpuser", &self->rpuser_handle) ||
        sh_handler("rank", &self->rank_handle)) {
        return 1;
    }
    if (player_init(self))
        return 1;
    if (playerdb_init(self))
        return 1; 
    return 0;
}

void
hall_main(struct service *s, int session, int source, int type, const void *msg, int sz) {
    struct hall *self = SERVICE_SELF;
    switch (type) {
    case MT_UM: {
        UM_CAST(UM_BASE, base, msg);
        switch (base->msgid) {
        case IDUM_ENTERHALL:
            player_main(s, source, NULL, msg, sz);
            break;
        case IDUM_HALL: {
            UM_CAST(UM_HALL, ha, msg);
            struct player *pr = sh_hash_find(&self->acc2player, ha->uid);
            if (pr == NULL)
                return;
            UM_CAST(UM_BASE, wrap, ha->wrap);
            switch (wrap->msgid) {
            case IDUM_LOGOUT:
                playerdb_send(s, pr, PDB_SAVE);
                player_main(s, source, pr, wrap, sz-sizeof(*ha));
                break;
            case IDUM_CHARCREATE:
                player_main(s, source, pr, wrap, sz-sizeof(*ha));
                break;
            case IDUM_ENTERROOM:
                playlogic_main(s, pr, wrap, sz-sizeof(*ha));
                break;
            default:
                if (wrap->msgid >= IDUM_AWARDB && wrap->msgid <= IDUM_AWARDE) {
                    rolelogic_main(s, pr, wrap, sz-sizeof(*ha));
                } else if (wrap->msgid >= IDUM_ROLEB && wrap->msgid <= IDUM_ROLEE) {
                    awardlogic_main(s, pr, wrap, sz-sizeof(*ha));
                } else if (wrap->msgid >= IDUM_RINGB && wrap->msgid <= IDUM_RINGE) {
                    ringlogic_main(s, pr, wrap, sz-sizeof(*ha));
                } else if ((wrap->msgid >= IDUM_PLAYB && wrap->msgid <= IDUM_PLAYE) ||
                           (wrap->msgid >= IDUM_PLAYB2 && wrap->msgid <= IDUM_PLAYE2)) {
                    playlogic_main(s, pr, wrap, sz-sizeof(*ha));
                }
            }
            break;
            }
        case IDUM_REDISREPLY: {
            UM_CAST(UM_REDISREPLY, rr, msg);
            playerdb_process_redis(s, rr, sz);
            break;
            }
        }
        break;
        }
    case MT_MONITOR:
        break;
    }
}
