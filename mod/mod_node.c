#include "sh.h"
#include "args.h"
#include "msg.h"
#include <arpa/inet.h>

#define NODE_MAX 256

struct int_vector {
    int cap;
    int sz;
    int *pi;
};

struct node {
    int connid;
    int node_handle;
    uint64_t last_hb_send_time;
    struct sh_node_addr addr; 
    struct int_vector handles;
};

struct remote {
    int hb_tick;
    int center_handle;
    int myid;
    struct node nodes[NODE_MAX];
    struct sh_array subs;
    struct sh_array pubs;
};

// sub pub cache
struct sub_ent {
    char *name;
};

struct pub_ent {
    int handle;
    char *name;
};

static void
cache_sub(struct sh_array *subs, const char *name) {
    struct sub_ent *ent;
    int i;
    for (i=0; i<subs->nelem; ++i) {
        ent = sh_array_get(subs, i);
        if (!strcmp(ent->name, name))
            return;
    }
    ent = sh_array_push(subs);
    ent->name = strdup(name);
}

static void
cache_pub(struct sh_array *pubs, const char *name, int handle) {
    struct pub_ent *ent;
    int i;
    for (i=0; i<pubs->nelem; ++i) {
        ent = sh_array_get(pubs, i);
        if (!strcmp(ent->name, name))
            return;
    }
    ent = sh_array_push(pubs);
    ent->handle = handle;
    ent->name = strdup(name);
}

static void
cache_init(struct remote *self) {
    sh_array_init(&self->subs, sizeof(struct sub_ent), 1);
    sh_array_init(&self->pubs, sizeof(struct pub_ent), 1);
}

static int
cache_free_sub(void *ent, void *ud) {
    free(((struct sub_ent*)ent)->name);
    return 0;
}

static int
cache_free_pub(void *ent, void *ud) {
    free(((struct pub_ent*)ent)->name);
    return 0;
}

static void
cache_fini(struct remote *self) {
    sh_array_foreach(&self->subs, cache_free_sub, NULL);
    sh_array_foreach(&self->pubs, cache_free_pub, NULL);
    sh_array_fini(&self->subs);
    sh_array_fini(&self->pubs);
}

// node 
static inline void
_update_node(struct node *no, 
        const char *naddr, int nport, 
        const char *gaddr, int gport,
        const char *waddr) {
    sh_strncpy(no->addr.naddr, naddr, sizeof(no->addr.naddr));
    sh_strncpy(no->addr.gaddr, gaddr, sizeof(no->addr.gaddr));
    sh_strncpy(no->addr.waddr, waddr, sizeof(no->addr.waddr));
    no->addr.nport = nport;
    no->addr.gport = gport;
}

static inline void
_bound_node_entry(struct node *no, int handle) {
    no->node_handle = handle;
}

static inline void
_bound_node_connection(struct node *no, int connid) {
    no->connid = connid;
    no->last_hb_send_time = sh_timer_now();
}

static int
_bound_handle_to_node(struct node *no, int handle) {
    struct int_vector *handles = &no->handles;
    int i;
    for (i=0; i<handles->sz; ++i) {
        if (handles->pi[i] == handle) {
            return 1;
        }
    }
    if (handles->sz >= handles->cap) {
        handles->cap *= 2;
        if (handles->cap == 0)
            handles->cap = 1;
        handles->pi = realloc(handles->pi, sizeof(handles->pi[0]) * handles->cap);
    }
    handles->pi[handles->sz++] = handle;
    return 0;
}

static inline struct node *
_get_node(struct remote *self, int id) {
    return (id>0 && id<NODE_MAX) ? &self->nodes[id] : NULL;
}

static inline struct node *
_my_node(struct remote *self) {
    return _get_node(self, self->myid);
}

static inline int
center_id(struct remote *self) {
    return sh_nodeid_from_handle(self->center_handle);
}

static bool
is_center(struct remote *self) {
    return center_id(self) == 0;
}

static void
_disconnect_node(struct module *s, int connid) {
    struct remote *self = MODULE_SELF;
    struct node *no = NULL;
    int i;
    for (i=0; i<NODE_MAX; ++i) {
        if (self->nodes[i].connid == connid) {
            no = &self->nodes[i];
            break;
        }
    }
    if (no) {
        int id = no-self->nodes;
        sh_info("Node(%d:%d) disconnect", id, connid);
        no->connid = -1;
        no->node_handle = -1;
        for (i=0; i<no->handles.sz; ++i) {
            sh_handle_exit(no->handles.pi[i]);
        }
        no->handles.sz = 0;
        if (is_center(self)) {
            sh_handle_vsend(MODULE_ID, self->center_handle, "UNREG %d", id);
        }
    }
}

// net
static void *
_block_read(int id, int *msgsz, int *err) {
    for (;;) {
        *err = 0;
        struct mread_buffer buf;
        int nread = sh_net_read(id, &buf, err);
        if (nread > 0) {
            if (buf.sz > 6) {
                void *msg = buf.ptr+6;
                int sz = sh_from_littleendian16((uint8_t*)buf.ptr) + 2;
                if (buf.sz >= sz) {
                    buf.ptr += sz;
                    buf.sz -= sz;
                    int drop = nread - buf.sz;
                    if (drop) {
                        sh_net_dropread(id, nread-buf.sz);
                    }
                    *msgsz = sz-6;
                    return msg;
                }
            }
        } else if (nread == 0) {
            continue;
        } else {
            return NULL;
        }
    }
}

static int
_dsend(struct remote *self, int connid, int source, int dest, int type, const void *msg, int sz) {
    if (sz <= UM_MAXSZ) {
        int len = sz+6;
        source &= 0x00ff;
        source |= (self->myid << 8);
        dest   &= 0x00ff;
        dest   |= (type << 8);
        uint8_t *tmp = malloc(len);
        sh_to_littleendian16(len-2, tmp);
        sh_to_littleendian16(source, tmp+2);
        sh_to_littleendian16(dest, tmp+4);
        memcpy(tmp+6, msg, sz);
        return sh_net_send(connid, tmp, len);
    } else {
        sh_error("Too large msg from %04x to %04x", source, dest);
        return 1;
    }
}

static int
_vdsend(struct remote *self, int connid, int source, int dest, const char *fmt, ...) {
    char msg[UM_MAXSZ];
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n >= sizeof(msg)) {
        sh_error("Too large msg %s from %04x to %04x", fmt, source, dest);
        return 1;
    }
    return _dsend(self, connid, source, dest, MT_TEXT, msg, n);
}


static int
_send(struct remote *self, int source, int dest, int type, const void *msg, size_t sz) {
    if (dest <= 0) {
        sh_error("Invalid dest %04x", dest);
        return 1;
    }
    int id = sh_nodeid_from_handle(dest);
    struct node *no = _get_node(self, id);
    if (no == NULL) {
        sh_error("Invalid nodeid from dest %04x", dest);
        return 1;
    }
    if (no->connid != -1) {
        return _dsend(self, no->connid, source, dest, type, msg, sz);
    } else {
        sh_error("Node(%d) has not connect, by dest %04x", id, dest);
        return 1;
    } 
}

static int
_vsend(struct remote *self, int source, int dest, const char *fmt, ...) {
    char msg[UM_MAXSZ];
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n >= sizeof(msg)) {
        sh_error("Too large msg %s from %04x to %04x", fmt, source, dest);
        return 1;
    }
    return _send(self, source, dest, MT_TEXT, msg, n);
}

// initialize
static int
_init_mynode(struct remote *self) {
    self->myid = sh_getint("node_id", 0);
    struct node *no = _my_node(self);
    if (no == NULL) {
        sh_error("Invalid node_id %d", self->myid);
        return 1;
    }
    _update_node(no, 
            sh_getstr("node_ip", "0"), 
            sh_getint("node_port", 0),
            sh_getstr("gate_ip", "0"), 
            sh_getint("gate_port", 0),
            sh_getstr("wan_ip", "0"));
    return 0;
}

static int
_listen(struct module *s) {
    struct remote *self = MODULE_SELF;
    struct node *my = _my_node(self);
    assert(my);
    int err;
    int id = sh_net_listen(my->addr.naddr, my->addr.nport, 0, s->moduleid, 0, &err);
    if (id == -1) {
        sh_error("Node listen on %s:%d err: %s", my->addr.naddr, my->addr.nport, sh_net_error(err));
        return 1;
    }
    sh_info("Node listen on %s:%u [%d]", my->addr.naddr, my->addr.nport, id);
    return 0;
}

static int
_send_center_entry(struct module *s, int id) {
    struct remote *self = MODULE_SELF;
    if (_vdsend(self, id, 0, 0, "%d %d", 
                sh_handleid(self->myid, self->center_handle),
                sh_handleid(self->myid, MODULE_ID))) {
        return 1;
    }
    return 0;
}

static int
_block_read_center_entry(int id, int *center_handle, int *node_handle) {
    int err;
    int sz;
    void *msg = _block_read(id, &sz, &err);
    if (msg == NULL) {
        sh_error("Recv center entry fail: %s", sh_net_error(err));
        return 1;
    }
    struct args A;
    args_parsestrl(&A, 2, msg, sz);
    if (A.argc == 2) {
        *center_handle = strtol(A.argv[0], NULL, 10);
        *node_handle = strtol(A.argv[1], NULL, 10);
        return 0;
    } else {
        sh_error("Recv invaild center entry");
        return 1;
    }
}

static int
_connect_node(struct module *s, struct node *no) {
    struct remote *self = MODULE_SELF;
    int id = no - self->nodes;
    if (no->connid != -1) {
        return 0;
    }
    struct sh_node_addr *addr = &no->addr; 
    int err; 
    int connid = sh_net_block_connect(addr->naddr, addr->nport, MODULE_ID, 0, &err);
    if (connid < 0) {
        sh_error("Connect node(%d) %s:%u fail: %s", 
                id, addr->naddr, addr->nport, sh_net_error(err));
        return 1;
    }
    sh_net_subscribe(connid, true);
    _bound_node_connection(no, connid); 

    sh_info("Connect to node(%d:%d) %s:%u ok", id, connid, addr->naddr, addr->nport);
    return 0;
}

static int
_connect_module(struct module *s, const char *name, int handle) {
    struct remote *self = MODULE_SELF;
    struct node *no;
    int id;
    id = sh_nodeid_from_handle(handle);
    if (id == 0)
        id = self->myid;
    no = _get_node(self, id);
    if (no == NULL) {
        sh_error("Connect module %s:%d fail: Invalid node(%d)", name, handle, id);
        return 1;
    }
    if (id != self->myid) {
        _connect_node(s, no);
    } else {
        handle = sh_moduleid_from_handle(handle);
    }
    if (!_bound_handle_to_node(no, handle)) {
        sh_handle_start(name, handle, &no->addr);
    }
    return 0;
}

static int
_broadcast_node(struct module *s, int id) {
    struct remote *self = MODULE_SELF;
    struct node *no = _get_node(self, id);
    if (no == NULL) {
        return 1;
    }
    if (no->connid == -1) {
        return 1;
    }
    int i;
    // boradcast me
    for (i=0; i<NODE_MAX; ++i) {
        struct node *other = &self->nodes[i];
        if (i == id || i == self->myid) 
            continue;
        if (other->connid == -1) 
            continue;
        _vsend(self, MODULE_ID, other->node_handle, "ADDR %d %s %u %s %u %s %04x",
                id, 
                no->addr.naddr, no->addr.nport, 
                no->addr.gaddr, no->addr.gport,
                no->addr.waddr, no->node_handle);
    }

    // get other
    for (i=0; i<NODE_MAX; ++i) {
        struct node *other = &self->nodes[i];
        if (i == id)
            continue;
        if (other->connid == -1 ||
            no->connid == -1)
            continue;
        _vsend(self, MODULE_ID, no->node_handle, "ADDR %d %s %u %s %u %s %04x",
                i, 
                other->addr.naddr, other->addr.nport, 
                other->addr.gaddr, other->addr.gport,
                other->addr.waddr, no->node_handle);
    }
    return 0;
}

static int
_connect_to_center(struct module* s) {
    struct remote *self = MODULE_SELF;

    const char *addr = sh_getstr("center_ip", "0");
    int port = sh_getint("center_port", 0);
    int err; 
    int connid = sh_net_block_connect(addr, port, MODULE_ID, 0, &err);
    if (connid < 0) {
        sh_error("Connect to center fail: %s", sh_net_error(err));
        return 1;
    }
    int center_handle, node_handle;
    if (_block_read_center_entry(connid, &center_handle, &node_handle)) {
        return 1;
    }
    int center_id = sh_nodeid_from_handle(center_handle);
    struct node *no = _get_node(self, center_id);
    if (no == NULL) {
        sh_error("Reg center node fail");
        return 1;
    }
    _update_node(no, addr, port, "", 0, "");
    sh_net_subscribe(connid, true);
    _bound_node_connection(no, connid);
    _bound_node_entry(no, node_handle);
    self->center_handle = center_handle;

    int self_handle = sh_handleid(self->myid, MODULE_ID);
    struct node *me = _my_node(self);
    if (_vsend(self, self_handle, center_handle, "REG %d %s %u %s %u %s %04x",
                self->myid, 
                me->addr.naddr, me->addr.nport, 
                me->addr.gaddr, me->addr.gport, 
                me->addr.waddr,
                self_handle)) {
        sh_error("Reg self to center fail");
        return 1;
    }
    sh_info("Connect to center(%d) %s:%u ok", center_id, addr, port);
    return 0;
}

// module
static inline int
_subscribe_module(struct module *s, const char *name) {
    struct remote *self = MODULE_SELF;
     
    int handle = module_query_id(name);
    if (handle != -1) {
        _connect_module(s, name, handle); // if local has mod also, connect it
    }
    return sh_handle_vsend(MODULE_ID, self->center_handle, "SUB %s", name);
}

static inline int
_publish_module(struct module *s, const char *name, int handle) {
    struct remote *self = MODULE_SELF;
 
    handle &= 0xff;
    handle |= (self->myid << 8) & 0xff00;
    return sh_handle_vsend(MODULE_ID, self->center_handle, "PUB %s:%04x", name, handle);
}

struct remote *
node_create() {
    struct remote* self = malloc(sizeof(*self));
    memset(self, 0, sizeof(*self));
    self->center_handle = -1;
    int i;
    for (i=0; i<NODE_MAX; ++i) {
        self->nodes[i].connid = -1;
        self->nodes[i].node_handle = -1;
    }
    return self;
}

void
node_free(struct remote* self) {
    if (self == NULL)
        return;

    cache_fini(self); 
    struct node *no;
    int i;
    for (i=0; i<NODE_MAX; ++i) {
        no = &self->nodes[i];
        if (no->handles.pi) {
            free(no->handles.pi);
            no->handles.pi = NULL;
            no->handles.sz = 0;
            no->handles.cap = 0;
        }
    }
    free(self);
}

int
node_init(struct module* s) {
    struct remote *self = MODULE_SELF;
    if (_init_mynode(self)) {
        return 1;
    }
    if (_listen(s)) {
        return 1;
    }
    cache_init(self); 
    self->center_handle = module_query_id("centers");
    if (self->center_handle == -1) {
        if (_connect_to_center(s)) {
            return 1;
        }
    }
    int hb_tick = sh_getint_inrange("node_heartbeat", 3, 30);
    self->hb_tick = hb_tick * 1000;
    sh_timer_register(MODULE_ID, self->hb_tick);
    return 0;
}

static void
r_read(struct module *s, struct net_message *nm) {
    int id = nm->connid;
    int err = 0; 
    struct mread_buffer buf;
    int nread = sh_net_read(id, &buf, &err); 
    if (nread > 0) {
        for (;;) {
            if (buf.sz < 6) {
                break;
            }
            uint16_t msgsz = sh_from_littleendian16((uint8_t*)buf.ptr) + 2;
            if (msgsz <= 6 || (msgsz-6) > UM_MAXSZ) {
                err = NET_ERR_MSG;
                break;
            }
            if (buf.sz < msgsz) {
                break;
            }
            uint16_t source = sh_from_littleendian16((uint8_t*)buf.ptr+2);
            uint16_t dest = sh_from_littleendian16((uint8_t*)buf.ptr+4);
            int type = (dest>>8) & 0xff;
            dest &= 0xff;
            sh_handle_send(source, dest, type, buf.ptr+6, msgsz-6);
            buf.ptr += msgsz;
            buf.sz  -= msgsz;
        }
        int drop = nread - buf.sz;
        if (drop) {
            sh_net_dropread(id, drop);
        }
    } else if (nread < 0) {
        goto errout;
    }
    return;
errout:
    nm->type = NETE_SOCKERR;
    nm->error = err;
    module_net(nm->ud, nm);
}

void
node_send(struct module *s, int session, int source, int dest, int type, const void *msg, int sz) {
    struct remote *self = MODULE_SELF;
    _send(self, source, dest, type, msg, sz);
}

void
node_net(struct module* s, struct net_message* nm) {
    struct remote *self = MODULE_SELF;
    switch (nm->type) {
    case NETE_READ:
        r_read(s, nm);
        break;
    case NETE_ACCEPT:
        if (is_center(self)) {
            _send_center_entry(s, nm->connid);
        }
        sh_net_subscribe(nm->connid, true);
        break;
    //case NETE_CONNECT:
        //sh_info("connect to node ok, %d", nm->connid);
        //break;
    //case NETE_CONNERR:
        //sh_error("connect to node fail: %s", sh_net_error(nm->error));
        //break;
    case NETE_SOCKERR:
        sh_error("node disconnect: %s, %d", sh_net_error(nm->error), nm->connid);
        _disconnect_node(s, nm->connid);
        break;
    default:
        sh_error("node unknown net event %d, %d", nm->type, nm->connid);
        break;
    }
}

void
node_time(struct module* s) {
    struct remote *self = MODULE_SELF;

    uint64_t now = sh_timer_now();
    struct node *no;
    int i;
    for (i=0; i<NODE_MAX; ++i) {
        no = &self->nodes[i];
        if (no->connid != -1 &&
            no->node_handle != -1) {
            if (now - no->last_hb_send_time >= self->hb_tick) {
                _dsend(self, no->connid, MODULE_ID, no->node_handle, MT_TEXT, "HB", 2);
            }
        }
    }

    int id = center_id(self);
    if (id > 0) {
        no = _get_node(self, id);
        if (no && no->connid == -1) {
            if (!_connect_to_center(s)) {
                struct sub_ent *se;
                for (i=0; i<self->subs.nelem; ++i) {
                    se = sh_array_get(&self->subs, i);
                    _subscribe_module(s, se->name);
                }
                struct pub_ent *pe;
                for (i=0; i<self->pubs.nelem; ++i) {
                    pe = sh_array_get(&self->pubs, i);
                    _publish_module(s, pe->name, pe->handle);
                }
            }
        }
    }
}

void
node_main(struct module *s, int session, int source, int type, const void *msg, int sz) {
    if (type != MT_TEXT)
        return;

    struct remote *self = MODULE_SELF;
    struct args A;
    if (args_parsestrl(&A, 0, msg, sz) < 1)
        return;

    const char *cmd = A.argv[0];

    if (!strcmp(cmd, "REG")) {
        if (A.argc != 8)
            return;
        int id = strtol(A.argv[1], NULL, 10);
        const char *naddr = A.argv[2];
        int nport = strtol(A.argv[3], NULL, 10);
        const char *gaddr = A.argv[4];
        int gport = strtol(A.argv[5], NULL, 10);
        const char *waddr = A.argv[6];
        int node_handle = strtol(A.argv[7], NULL, 16);
        if (id <= 0) {
            sh_error("Reg invalid node(%d)", id);
            return;
        }
        if (id == self->myid) {
            sh_error("Reg self node(%d)", id);
            return;
        }
        struct node *no = _get_node(self, id);
        if (no) {
            if (no->connid != -1) {
                sh_error("Node(%d:%d) has connected", id, no->connid);
            } else {
                _update_node(no, naddr, nport, gaddr, gport, waddr);
                _bound_node_entry(no, node_handle);
                _connect_node(s, no);
            } 
        }
    } else if (!strcmp(cmd, "ADDR")) {
        if (A.argc != 8)
            return;
        int id = strtol(A.argv[1], NULL, 10);
        const char *naddr = A.argv[2];
        int nport = strtol(A.argv[3], NULL, 10);
        const char *gaddr = A.argv[4];
        int gport = strtol(A.argv[5], NULL, 10);
        const char *waddr = A.argv[6];
        int node_handle = strtol(A.argv[7], NULL, 16);
        if (id > 0) {
            struct node *no = _get_node(self, id);
            if (no) {
                _update_node(no, naddr, nport, gaddr, gport, waddr);
                _bound_node_entry(no, node_handle);
                // no need connect each other
                //_connect_node(s, no); 
            }
        }
    } else if (!strcmp(cmd, "BROADCAST")) {
        if (A.argc != 2)
            return;
        int id = strtol(A.argv[1], NULL, 10);
        _broadcast_node(s, id);
    } else if (!strcmp(cmd, "SUB")) {
        if (A.argc != 2)
            return;
        const char *name = A.argv[1];
        cache_sub(&self->subs, name);
        _subscribe_module(s, name); 
    } else if (!strcmp(cmd, "PUB")) {
        if (A.argc != 2)
            return;
        const char *name = A.argv[1];
        char *p = strchr(name, ':');
        if (p) {
            p[0] = '\0';
            int handle = strtol(p+1, NULL, 16); 
            cache_pub(&self->pubs, name, handle);
            _publish_module(s, name, handle);
        }
    } else if (!strcmp(cmd, "HANDLE")) { // after subscribe
        if (A.argc != 2)
            return;
        const char *name = A.argv[1];
        char *p = strchr(name, ':');
        if (p) {
            p[0] = '\0';
            int handle = strtol(p+1, NULL, 16); 
            _connect_module(s, name, handle);
        }
    } else if (!strcmp(cmd, "HANDLES")) { // before subscribe
        if (A.argc != 2)
            return;
        const char *name = A.argv[1];
        sh_trace("Node HANNDLES %s", name);
        char *p = strchr(name, ':');
        if (p) {
            p[0] = '\0';
        } 
        sh_handle_startb(name);
        if (p) {
            char* saveptr = NULL;
            char* one = strtok_r(p+1, ",", &saveptr);
            while (one) {
                int handle = strtol(one, NULL, 16);
                _connect_module(s, name, handle);
                one = strtok_r(NULL, ",", &saveptr);
            }
        }
        sh_handle_starte(name);
    }
}
