#include "robot_tplt.h"
#include "robot.h"
#include "sh.h"

static int
load_tplt(struct robot *self) {
    tplt_free(self->T);
#define TBLFILE(name) "./res/tbl/"#name".tbl"
    struct tplt_desh desh[] = {
        { TPLT_ROLE, sizeof(struct role_tplt), 1, TBLFILE(role), 0, TPLT_VIST_VEC32},
    };
    self->T = tplt_create(desh, sizeof(desh)/sizeof(desh[0]));
    return self->T ? 0 : 1;
}

int
robot_tplt_init(struct robot *self) {
    if (load_tplt(self)) {
        return 1;
    }
    return 0;
}

void
robot_tplt_fini(struct robot *self) {
    if (self->T) {
        tplt_free(self->T);
        self->T = NULL;
    }
}

void
robot_tplt_main(struct module *s, int session, int source, int type, const void *msg, int sz) {
    struct robot *self = MODULE_SELF;
    if (type != MT_TEXT)
        return;

    if (!strncmp("reload", msg, sz)) {
        if (!load_tplt(self)) {
            sh_info("reload tplt ok");
        } else {
            sh_error("reload tplt fail");
        }
    }
}
