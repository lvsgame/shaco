#include "sc_service.h"
#include "sc_env.h"
#include "sh_monitor.h"
#include "sh_util.h"
#include "sc.h"
#include "sc_log.h"
#include "sc_node.h"
#include "sc_timer.h"
#include "sh_hash.h"
#include "user_message.h"
#include "cli_message.h"
#include "attrilogic.h"
#include "tplt_include.h"
#include "tplt_struct.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>

// Accid  reserve 1001~1000000, create from 1000001
// Charid reserve 1001~1000000, create from 1000001
// Create robot, insert to db
// Load  robot to memory
// Apply robot to match
// Login robot to room
// Award score, exp
// Rank score
// Rank reset

#define ACCID_BEGIN 1001
#define ACCID_END 1000000
#define CHARID_BEGIN 1001
#define CHARID_END 1000000
#define ROBOT_MAX       min((CHARID_END-CHARID_BEGIN+1), (ACCID_END-ACCID_BEGIN+1))

#define S_REST   0
#define S_WAIT   1
#define S_FIGHT  2

#define UID(ag) ((ag)->data.accid)

struct agent { 
    int status;
    int level;
    struct chardata data;
    uint32_t last_change_role_time;
    struct agent *next;
};

struct robot {
    int match_handle;
    int room_handle;
    int nagent;
    struct agent *rest_head; 
    struct agent *rest_tail;
    struct sh_hash agents;
};

static struct agent *
alloc_agent(struct robot *self) {
    struct agent *ag = malloc(sizeof(*ag));
    memset(ag, 0, sizeof(*ag));
    ag->status = S_REST;
    return ag;
}

static struct agent *
agent_pull(struct robot *self) {
    struct agent *ag = self->rest_head;
    if (ag == NULL)
        return NULL;
    assert(ag->status == S_REST);
    ag->status = S_WAIT;
    self->rest_head = self->rest_head->next;
    return ag;
}

static void
agent_fight(struct robot *self, struct agent *ag) {
    assert(ag->status == S_WAIT);
    ag->status = S_FIGHT;
}

static void
agent_rest(struct robot *self, struct agent *ag) {
    ag->status = S_REST;
    if (self->rest_head) {
        assert(self->rest_tail != NULL);
        assert(self->rest_tail->next == NULL);
        self->rest_tail->next = ag;
    } else {
        self->rest_head = ag;
    }
    self->rest_tail = ag;
    ag->next = NULL;
}

static inline uint32_t
rand_role() {
    const struct tplt_holder* holder = tplt_get_holder(TPLT_ROLE);
    if (holder) {
        int n = TPLT_HOLDER_NELEM(holder);
        if (n > 0) {
            int idx = rand()%n;
            const struct role_tplt *tplt = TPLT_HOLDER_FIRSTELEM(role_tplt, holder); 
            return tplt[idx].id;
        }
    }
    return 0;
}

static inline void
init_agent_data(struct agent *ag, int idx, int level) {
    struct chardata *cdata = &ag->data;
    ag->level = level;
    cdata->charid = CHARID_BEGIN+idx;
    snprintf(cdata->name, sizeof(cdata->name), "wabao%02d_%d", level, cdata->charid);
    cdata->accid = ACCID_BEGIN+idx;
    cdata->role = rand_role();
    attrilogic_main(cdata);
}

static int
init_agents(struct robot *self) {
    sh_hash_init(&self->agents, 1);
    // todo
    int count = 100;
    if (count > ROBOT_MAX) {
        count = ROBOT_MAX;
    }
    int i;
    for (i=0; i<count; ++i) {
        struct agent *ag = alloc_agent(self);
        if (ag == NULL) {
            return 1;
        }
        init_agent_data(ag, i, rand()%10+1);
        agent_rest(self, ag);
        sh_hash_insert(&self->agents, UID(ag), ag);
    }
    self->nagent = count;
    return 0;
}

static inline void
build_brief(struct agent *ag, struct tmemberbrief *brief) {
    struct chardata *cdata = &ag->data;
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
build_detail(struct agent *ag, struct tmemberdetail *detail) {
    struct chardata *cdata = &ag->data;
    detail->accid = cdata->accid;
    detail->charid = cdata->charid;
    memcpy(detail->name, cdata->name, sizeof(cdata->name));
    detail->role = cdata->role;
    detail->skin = cdata->skin;
    detail->score_dashi = cdata->score_dashi;
    detail->attri = cdata->attri;
}

static void
pull(struct service *s, int source, int count) {
    struct robot *self = SERVICE_SELF;
    UM_DEFFIX(UM_ROBOT_APPLY, ra);
    struct agent *ag = agent_pull(self);
    if (ag) {
        build_brief(ag, &ra->brief);
        sh_service_send(SERVICE_ID, source, MT_UM, ra, sizeof(*ra)); 
        sc_trace("Robot %u pull", UID(ag));
    } else {
        sc_trace("Robot none to pull");
    }
}

static void
play_fail(struct service *s, struct agent *ag) {
    struct robot *self = SERVICE_SELF;
    if (ag->status == S_WAIT) { 
        agent_rest(self, ag);
        sc_trace("Robot %u rest, because play fail", UID(ag));
    } else {
        sc_trace("Robot %u rest, because play fail, but status %d", UID(ag), ag->status);
    }
}

static void
enter_room(struct service *s, struct agent *ag, struct UM_ENTERROOM *er) {
    struct robot *self = SERVICE_SELF;
    if (ag->status != S_WAIT) {
        sc_trace("Robot %u receive enter room, but status %d", UID(ag), ag->status);
        return;
    }
    if (sc_service_has(self->room_handle, er->room_handle)) {
        agent_fight(self, ag);
        UM_DEFFIX(UM_ROBOT_LOGINROOM, lr);
        lr->roomid = er->roomid;
        lr->level = ag->level;
        build_detail(ag, &lr->detail);
        sh_service_send(SERVICE_ID, er->room_handle, MT_UM, lr, sizeof(*lr));
        sc_trace("Robot %u send enter room to handle %x", UID(ag), er->room_handle);
    } else {
        agent_rest(self, ag);
        sc_trace("Robot %u receive enter handle %x not exist", UID(ag), er->room_handle);
    }
}

static void
exit_room(struct service *s, uint32_t uid) {
    struct robot *self = SERVICE_SELF;
    struct agent *ag = sh_hash_find(&self->agents, uid);
    if (ag == NULL) {
        return;
    }
    if (ag->status == S_FIGHT) {
        agent_rest(self, ag);
        UM_DEFWRAP(UM_MATCH, ma, UM_LOGOUT, lo);
        ma->uid = UID(ag);
        lo->err = SERR_OK;
        sh_service_send(SERVICE_ID, self->match_handle, MT_UM, ma, sizeof(*ma)+sizeof(*lo));
        sc_trace("Robot %u notify match exit room", UID(ag));
    } else {
        sc_trace("Robot %u receive exit room, but status %d", UID(ag), ag->status);
    }
}

struct robot*
robot_create() {
    struct robot* self = malloc(sizeof(*self));
    memset(self, 0, sizeof(*self));
    return self;
}

void
robot_free(struct robot* self) {
    if (self == NULL)
        return;
    sh_hash_foreach(&self->agents, free);
    sh_hash_fini(&self->agents);
    self->rest_head = NULL;
    self->nagent = 0;
    free(self);
}

int
robot_init(struct service* s) {
    struct robot* self = SERVICE_SELF;
    if (!service_isprepared(sc_getstr("tplt_handle", ""))) {
        return 1;
    }
    if (sh_handle_publish(SERVICE_NAME, PUB_SER)) {
        return 1;
    }
    struct sh_monitor_handle h = {SERVICE_ID, SERVICE_ID};
    if (sh_monitor("match", &h, &self->match_handle) ||
        sh_monitor("room", &h, &self->room_handle)) {
        return 1;
    }
    if (init_agents(self)) {
        return 1;
    } 
    return 0;
}

void
robot_main(struct service *s, int session, int source, int type, const void *msg, int sz) {
    struct robot *self = SERVICE_SELF;
    switch (type) {
    case MT_UM: {
        UM_CAST(UM_BASE, base, msg);
        switch (base->msgid) {
        case IDUM_ROBOT_PULL: {
            UM_CAST(UM_ROBOT_PULL, rp, msg);
            pull(s, source, rp->count);
            break;
            }
        case IDUM_MATCH: {
            UM_CAST(UM_MATCH, ma, msg);
            struct agent *ag = sh_hash_find(&self->agents, ma->uid);
            if (ag == NULL) {
                return;
            }
            UM_CAST(UM_BASE, sub, ma->wrap);
            switch (sub->msgid) {
            case IDUM_PLAYFAIL:
                play_fail(s, ag);
                break;
            case IDUM_ENTERROOM: {
                UM_CAST(UM_ENTERROOM, er, ma->wrap);
                enter_room(s, ag, er);
                break;
                }
            }
            break;
            }
        case IDUM_EXITROOM: {
            UM_CAST(UM_EXITROOM, exit, msg);
            exit_room(s, exit->uid);
            break;
            }
        }
        break;
        }
    }
}