#include "host_service.h"
#include "host.h"
#include "host_node.h"
#include "host_dispatcher.h"
#include "host_gate.h"
#include "host_log.h"
#include "host_timer.h"
#include "host_assert.h"
#include "redis.h"
#include "cli_message.h"
#include "user_message.h"
#include "node_type.h"
#include "memrw.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#define STATE_INVALID   0
#define STATE_CONNECTED 1
#define STATE_LOGIN     2
#define STATE_VERIFYED  3
#define STATE_GATELOAD  4
#define STATE_DONE      5

#define VERIFY_OK 0
#define VERIFY_FAIL 1
#define ERR_NOREDIS 2
#define ERR_REDIS 3
#define ERR_NOREGION 4
#define ERR_LOAD 5
#define ERR_TIMEOUT 6
#define ERR_REG 7

struct player {
    int state;  // see STATE_*
    uint32_t accid;
    uint64_t key;
    char account[ACCOUNT_NAME_MAX];
    char passwd[ACCOUNT_PASSWD_MAX];
};

struct login {
    int pmax;
    struct player* players;
    struct redis_reply reply;
    uint32_t key;
};

struct login*
login_create() {
    struct login* self = malloc(sizeof(*self));
    memset(self, 0, sizeof(*self));
    return self;
}

void
login_free(struct login* self) {
    redis_finireply(&self->reply);
    free(self);
}

int
login_init(struct service* s) {
    struct login* self = SERVICE_SELF;
    int pmax = host_gate_maxclient();
    if (pmax == 0) {
        host_error("maxclient is zero, try load service gate before this");
        return 1;
    }
    self->pmax = pmax;
    self->players = malloc(sizeof(struct player) * pmax);
    memset(self->players, 0, sizeof(struct player) * pmax);
   
    redis_initreply(&self->reply, 512, 0);
    SUBSCRIBE_MSG(s->serviceid, IDUM_ACCOUNTLOGINRES);
    SUBSCRIBE_MSG(s->serviceid, IDUM_MINLOADFAIL);
    SUBSCRIBE_MSG(s->serviceid, IDUM_ACCOUNTLOGINRES);
    return 0;
}

static inline struct player*
_getplayer(struct login* self, struct gate_client* c) {
    int id = host_gate_clientid(c);
    assert(id >= 0 && id < self->pmax);
    return &self->players[id];
}

static inline struct player*
_getonlineplayer(struct login* self, struct gate_client* c) {
    struct player* p = _getplayer(self, c);
    if (p && (p->state != STATE_INVALID))
        return p;
    return NULL;
}

static int
_query(struct login* self, struct gate_client* c, struct player* p) {
    const struct host_node* redisp = host_node_get(HNODE_ID(NODE_REDISPROXY, 0));
    if (redisp == NULL) {
        return 1;
    }
    UM_DEFVAR(UM_REDISQUERY, rq);
    rq->needreply = 1;
    struct memrw rw;
    memrw_init(&rw, rq->data, rq->msgsz - sizeof(*rq));
    memrw_write(&rw, (int*)&c->connid, sizeof(int));
    memrw_write(&rw, p->account, sizeof(p->account));
    rq->cbsz = RW_CUR(&rw);
    int len = snprintf(rw.ptr, RW_SPACE(&rw), "hmget user:%s id passwd\r\n", p->account);
    memrw_pos(&rw, len);
    rq->msgsz = sizeof(*rq) + RW_CUR(&rw);
    UM_SENDTONODE(redisp, rq, rq->msgsz);
    return 0;
}

static void
_logout(struct gate_client* c, struct player* p, int8_t result, bool active) {
    p->state = STATE_INVALID;
    // !!! dangers
    if (active) {
        UM_DEFFIX(UM_LOGINACCOUNTFAIL, fail);
        fail->error = result;
        UM_SENDTOCLI(c->connid, fail, fail->msgsz);
        // todo disconnect
        host_gate_disconnclient(c, true);
    }
}

static void
_login(struct login* self, struct gate_client* c, struct UM_BASE* um) {
    struct player* p = _getplayer(self, c);
    if (p == NULL ||
        p->state != STATE_INVALID) {
        host_gate_disconnclient(c, true);
        return;
    }
    if (_query(self, c, p)) {
        _logout(c, p, ERR_NOREDIS, true);
        return;
    }
    p->state = STATE_LOGIN;
}

void
login_usermsg(struct service* s, int id, void* msg, int sz) {
    struct login* self = SERVICE_SELF;
    struct gate_message* gm = msg;
    assert(gm->c);
    UM_CAST(UM_BASE, um, gm->msg);
    switch (um->msgid) {
    case IDUM_LOGINACCOUNT:
        _login(self, gm->c, um);
        break;
    }

}

static inline int
_checkvalue(struct redis_replyitem* f, const char* value) {
    int len = strlen(value);
    if (len == f->value.len) {
        return memcmp(f->value.p, value, len);
    }
    return 1;
}

static void
_handleredis(struct login* self, struct node_message* nm) {
    hassertlog(nm->um->msgid == IDUM_REDISREPLY);

    UM_CAST(UM_REDISREPLY, rep, nm->um);
    struct memrw rw;
    memrw_init(&rw, rep->data, rep->msgsz - sizeof(*rep));
    int cid;
    memrw_read(&rw, &cid, sizeof(cid));
    struct gate_client* c = host_gate_getclient(cid);
    if (c == NULL) {
        return; // maybe disconnect
    }
    struct player* p = _getonlineplayer(self, c);
    if (p == NULL) {
        return; // maybe disconnect, and other connect but not logined
    }
    char account[ACCOUNT_NAME_MAX];
    memrw_read(&rw, account, sizeof(account));
    if (memcmp(p->account, account, sizeof(account)) == 0) {
        return; // other
    }
    
    int result = ERR_REDIS;

    int sz = rep->msgsz - sizeof(*rep);
    redis_resetreplybuf(&self->reply, rep->data, sz);
    if (redis_getreply(&self->reply) == REDIS_SUCCEED) {
        struct redis_replyitem* item = self->reply.stack[0];
        if (item->type == REDIS_REPLY_ARRAY) {
            int n = item->value.i;
            if (n == 2) {
                if (_checkvalue(&item->child[1], p->passwd) == 0) {
                    uint32_t accid = redis_bulkitem_toul(&item->child[0]);
                    if (accid > 0) {
                        p->accid = accid;
                        p->key = (((uint64_t)p->accid) << 32) | (++self->key);
                        p->state = STATE_VERIFYED;
                        result = VERIFY_OK;
                    } else {
                        result = VERIFY_FAIL;
                    }
                } else {
                    result = VERIFY_FAIL;
                }
            }
        }
    }
    if (result == VERIFY_OK) {
        // todo: add multi region
        const struct host_node* region = host_node_get(HNODE_ID(NODE_LOAD, 0));
        if (region) {
            uint32_t ip;
            uint16_t port;
            host_net_socket_address(c->connid, &ip, &port);
            UM_DEFFIX(UM_ACCOUNTLOGINREG, reg);
            reg->accid = p->accid;
            reg->key = p->key;
            reg->clientip = ip;
            strncpy(reg->account, p->account, ACCOUNT_NAME_MAX);
            UM_SENDTONODE(region, reg, reg->msgsz);
            p->state = STATE_GATELOAD;
        } else {
            result = ERR_NOREGION;
        }
    }
    if (result != VERIFY_OK) {
        _logout(c, p, result, true);
    }
}

static void
_loginres(struct login* self, struct UM_BASE* um) {
    UM_CAST(UM_ACCOUNTLOGINRES, res, um);
    struct gate_client* c = host_gate_getclient(res->cid);
    if (c == NULL) {
        return; // maybe disconnect
    }
    struct player* p = _getonlineplayer(self, c);
    if (p == NULL) {
        return; // maybe disconnect, then other connected and not login
    }
    if (p->state == STATE_GATELOAD &&
        p->accid == res->accid &&
        p->key   == res->key) {
        if (res->ok) {
            UM_DEFFIX(UM_NOTIFYGATE, notify);
            notify->accid = p->accid;
            notify->key = p->key;
            notify->addr = res->addr;
            notify->port = res->port;
            UM_SENDTOCLI(c->connid, notify, notify->msgsz);
            // todo: disconnect
        } else {
            _logout(c, p, ERR_REG, true);
        }
    } else {
        return; // maybe disconnect, then other connected and not gateload 
    }
}

static void
_handleload(struct login* self, struct node_message* nm) {
    switch (nm->um->msgid) {
    case IDUM_MINLOADFAIL:
        // todo the region is invalid
        break;
    case IDUM_ACCOUNTLOGINRES:
        _loginres(self, nm->um);
        break;
    } 
}

void
login_nodemsg(struct service* s, int id, void* msg, int sz) {
    struct login* self = SERVICE_SELF;
    struct node_message nm;
    if (_decode_nodemessage(msg, sz, &nm)) {
        return;
    }
    switch (nm.hn->tid) {
    case NODE_REDISPROXY:
        _handleredis(self, &nm);
        break;
    case NODE_LOAD:
        _handleload(self, &nm);
        break;
    }
}

void
login_net(struct service* s, struct gate_message* gm) {
    struct login* self = SERVICE_SELF;
    struct net_message* nm = gm->msg;
    struct player* p = NULL;
    switch (nm->type) {
    case NETE_SOCKERR:
    case NETE_TIMEOUT:
        p = _getonlineplayer(self, gm->c);
        if (p) {
            _logout(gm->c, p, 0, false);
        }
        break;
    }
}

void
login_time(struct service* s) {
    struct login* self= SERVICE_SELF; 
    uint64_t now = host_timer_now(); 
   
    struct player* p;
    struct gate_client* c = host_gate_firstclient();
    int max = host_gate_maxclient();
    int i;
    for (i=0; i<max; ++i) {
        if (c[i].connid == -1) {
            continue;
        }
        if (now > c[i].create_time &&
            now - c[i].create_time > 10*1000) { // short connection mode
            p = _getonlineplayer(self, &c[i]);
            if (p) {
                _logout(&c[i], p, ERR_TIMEOUT, true);
            } else {
                host_gate_disconnclient(&c[i], true);
            }
        }
    }
}