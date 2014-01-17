#include "sc_service.h"
#include "sc.h"
#include "sc_log.h"
#include "sc_node.h"
#include "sc_timer.h"
#include "player.h"
#include "playerdb.h"
#include "tplt_include.h"
#include "tplt_struct.h"
#include "user_message.h"
#include "cli_message.h"
#include "attrilogic.h"
#include "hall.h"
#include <string.h>
#include <stdlib.h>

#define ROLE_DEF 10 // 默认给予ID

static bool
has_role(struct chardata* cdata, uint32_t roleid) {
    uint32_t typeid = ROLE_TYPEID(roleid);
    uint32_t clothid = ROLE_CLOTHID(roleid);
    if (typeid >= 0 && typeid < ROLE_MAX) {
        if (clothid >= 0 && clothid < ROLE_CLOTHES_MAX) {
            return cdata->ownrole[typeid] & (1<<clothid);
        }
    }
    return false;
}

static int
add_role(struct chardata* cdata, uint32_t roleid) {
    uint32_t typeid = ROLE_TYPEID(roleid);
    uint32_t clothid = ROLE_CLOTHID(roleid);
    if (typeid >= 0 && typeid < ROLE_MAX) {
        if (clothid >= 0 && clothid < ROLE_CLOTHES_MAX) {
            cdata->ownrole[typeid] |= 1<<clothid;
            return 0;
        }
    }
    return 0;
}

static void
sync_money(struct service *s, struct player* pr) {
    UM_DEFWRAP(UM_CLIENT, cl, UM_SYNCMONEY, sm);
    cl->uid  = UID(pr);
    sm->coin = pr->data.coin;
    sm->diamond = pr->data.diamond;
    sh_service_send(SERVICE_ID, pr->watchdog_source, MT_UM, cl, sizeof(*cl) + sizeof(*sm));
}

static void
sync_addrole(struct service *s, struct player* pr, uint32_t roleid) {
    UM_DEFWRAP(UM_CLIENT, cl, UM_ADDROLE, ar);
    cl->uid  = UID(pr);
    ar->roleid = roleid;
    sh_service_send(SERVICE_ID, pr->watchdog_source, MT_UM, cl, sizeof(*cl) + sizeof(*ar));
}

static inline void
use_role(struct chardata* cdata, const struct role_tplt* tplt) {
    cdata->role = tplt->id;
}

static void
process_userole(struct service *s, struct player *pr, const struct UM_USEROLE *use) {
    //struct rolelogic* self = SERVICE_SELF;

    struct chardata* cdata = &pr->data;
    uint32_t roleid  = use->roleid;

    if (roleid == cdata->role) {
        return;
    }
    const struct role_tplt* tplt = tplt_find(TPLT_ROLE, roleid);
    if (tplt == NULL) {
        return;
    }
    if (!has_role(cdata, roleid)) {
        return;
    }
    // do logic
    use_role(cdata, tplt);
  
    // refresh attribute
    attrilogic_main(s, pr, NULL, 0);

    hall_sync_role(s, pr);

    playerdb_send(s, pr, PDB_SAVE);
}

static void
process_buyrole(struct service *s, struct player *pr, const struct UM_BUYROLE *buy) {
    struct chardata* cdata = &pr->data;
    uint32_t roleid  = buy->roleid;
   
    if (has_role(cdata, roleid)) {
        return; // 已经拥有
    }
    const struct role_tplt* tplt = tplt_find(TPLT_ROLE, roleid);
    if (tplt == NULL) {
        return;
    }
    if (cdata->level < tplt->needlevel) {
        return; // 等级不足
    }
    if (cdata->coin < tplt->needcoin) {
        return; // 金币不足
    }
    if (cdata->diamond < tplt->needdiamond) {
        return; // 钻石不足
    }
    // do logic
    if (add_role(cdata, roleid)) {
        return;
    }
    cdata->coin -= tplt->needcoin;
    cdata->diamond -= tplt->needdiamond;
    sync_addrole(s, pr, roleid);
    sync_money(s, pr);

    playerdb_send(s, pr, PDB_SAVE);
}

static void
login(struct player* pr) {
    struct chardata* cdata = &pr->data;
    const struct role_tplt* tplt;
    if (cdata->role == 0) {
        tplt = NULL;
    } else {
        tplt = tplt_find(TPLT_ROLE, cdata->role);
    }
    if (tplt == NULL && 
        cdata->role != ROLE_DEF) {
        cdata->role = ROLE_DEF;
        tplt = tplt_find(TPLT_ROLE, cdata->role);
    }
    if (tplt) {
        if (!has_role(cdata, ROLE_DEF)) {
            add_role(cdata, ROLE_DEF);
        }
        use_role(cdata, tplt);
    } else {
        sc_error("can not found role %d, charid %u", cdata->role, cdata->charid);
    }
}

void
rolelogic_main(struct service *s, struct player *pr, const void *msg, int sz) {
    UM_CAST(UM_BASE, base, msg);
    switch (base->msgid) {
    case IDUM_ENTERHALL:
        login(pr);
        break;
    case IDUM_USEROLE: {
        UM_CASTCK(UM_USEROLE, use, base, sz);
        process_userole(s, pr, use);
        break;
        }
    case IDUM_BUYROLE: {
        UM_CASTCK(UM_BUYROLE, buy, base, sz);
        process_buyrole(s, pr, buy);
        break;
        }
    }
}
