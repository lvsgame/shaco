#include "sc_service.h"
#include "sc.h"
#include "sc_log.h"
#include "sc_node.h"
#include "sc_timer.h"
#include "player.h"
#include "tplt_include.h"
#include "tplt_struct.h"
#include "user_message.h"
#include "cli_message.h"
#include "hall.h"
#include <string.h>
#include <stdlib.h>

static inline void
build_brief(struct player *pr, struct tmemberbrief *brief) {
    struct chardata *cdata = &pr->data;
    brief->accid = cdata->accid;
    brief->charid = cdata->charid;
    memcpy(brief->name, cdata->name, sizeof(cdata->name));
    brief->role = cdata->role;
    brief->skin = cdata->skin;
    brief->oxygen = cdata->attri.oxygen;
    brief->body = cdata->attri.body;
    brief->quick = cdata->attri.quick;
}

static inline void
build_detail(struct player *pr, struct tmemberdetail *detail) {
    struct chardata *cdata = &pr->data;
    detail->accid = cdata->accid;
    detail->charid = cdata->charid;
    memcpy(detail->name, cdata->name, sizeof(cdata->name));
    detail->role = cdata->role;
    detail->skin = cdata->skin;
    detail->score_dashi = cdata->score_dashi;
    detail->attri = cdata->attri;
}

static void
play(struct service *s, struct player *pr, int type) {
    struct hall *self = SERVICE_SELF;
    if (pr->status == PS_HALL) { 
        pr->status = PS_WAITING;
        UM_DEFFIX(UM_APPLY, ap);
        ap->type = type;
        build_brief(pr, &ap->brief);
        sh_service_send(SERVICE_ID, self->match_handle, MT_UM, ap, sizeof(*ap));
        sc_trace("Play %u send play to match", UID(pr));
    } else {
        sc_trace("Play %u request play, but status %d", UID(pr), pr->status);
    }
}

static void
play_fail(struct service *s, struct player *pr, struct UM_PLAYFAIL *fail) {
    if (pr->status == PS_WAITING) { 
        pr->status = PS_HALL;
        UM_DEFWRAP(UM_CLIENT, cl, UM_PLAYFAIL, pl);
        cl->uid = UID(pr);
        *pl = *fail;
        sh_service_send(SERVICE_ID, pr->watchdog_source, MT_UM, cl, sizeof(*cl)+sizeof(*pl));
        sc_trace("Play %u notify client play fail", UID(pr));
    } else {
        sc_trace("Play %u receive play fail, but status %d", UID(pr), pr->status);
    }
}

static void
waiting(struct service *s, struct player *pr, struct UM_PLAYWAIT *wait) {
    if (pr->status == PS_WAITING) {
        UM_DEFWRAP(UM_CLIENT, cl, UM_PLAYWAIT, wt);
        cl->uid = UID(pr);
        *wt = *wait;
        sh_service_send(SERVICE_ID, pr->watchdog_source, MT_UM, cl, sizeof(*cl)+sizeof(*wt));
        sc_trace("Play %u notify client waiting", UID(pr));
    } else {
        sc_trace("Play %u receive waiting, but status %d", UID(pr), pr->status);
    }
}

static void
enter_room(struct service *s, struct player *pr, struct UM_ENTERROOM *er) {
    if (pr->status == PS_WAITING) { 
        pr->status = PS_ROOM;

        UM_DEFFIX(UM_LOGINROOM, lr);
        lr->room_handle = er->room_handle;
        lr->roomid = er->roomid;
        build_detail(pr, &lr->detail);
        sh_service_send(SERVICE_ID, pr->watchdog_source, MT_UM, lr, sizeof(*lr));
        sc_trace("Play %u notify client enter room", UID(pr));
    } else {
        sc_trace("Play %u receive enter room, but status %d", UID(pr), pr->status);
    }
}

static void
loading(struct service *s, struct player *pr, struct UM_PLAYLOADING *loading) {
    // just route to client loading
    UM_DEFWRAP(UM_CLIENT, cl, UM_PLAYLOADING, pl);
    cl->uid = UID(pr);
    *pl = *loading;
    sh_service_send(SERVICE_ID, pr->watchdog_source, MT_UM, cl, sizeof(*cl)+sizeof(*pl));
    sc_trace("Play %u notify client loading", UID(pr));
}

static void
exit_room(struct service *s, struct player *pr) {
    struct hall *self = SERVICE_SELF;
    if (pr->status == PS_ROOM) {
        pr->status = PS_HALL;
        UM_DEFWRAP(UM_MATCH, ma, UM_LOGOUT, lo);
        ma->uid = UID(pr);
        lo->err = SERR_OK;
        sh_service_send(SERVICE_ID, self->match_handle, MT_UM, ma, sizeof(*ma)+sizeof(*lo));
        sc_trace("Play %u notify match exit room", UID(pr));
    } else {
        sc_trace("Play %u receive exit room, but status %d", UID(pr), pr->status);
    }
}

void 
playlogic_main(struct service *s, struct player *pr, const void *msg, int sz) {
    UM_CAST(UM_BASE, base, msg);
    switch (base->msgid) {
    //case IDUM_ENTERHALL: {
        //login(pr);
        //break;
        //}
    case IDUM_PLAY: {
        UM_CASTCK(UM_PLAY, pl, base, sz);
        play(s, pr, pl->type);
        break;
        }
    case IDUM_PLAYFAIL: {
        UM_CASTCK(UM_PLAYFAIL, pf, base, sz);
        play_fail(s, pr, pf);
        break;
        }
    case IDUM_PLAYWAIT: {
        UM_CASTCK(UM_PLAYWAIT, pw, base, sz);
        waiting(s, pr, pw);
        break;
        }
    case IDUM_ENTERROOM: {
        UM_CASTCK(UM_ENTERROOM, er, base, sz);
        enter_room(s, pr, er);
        break;
        }
    case IDUM_PLAYLOADING: {
        UM_CASTCK(UM_PLAYLOADING, pl, base, sz);
        loading(s, pr, pl);
        break;
        }
    case IDUM_EXITROOM: {
        exit_room(s, pr);
        break;
        }
    }
}