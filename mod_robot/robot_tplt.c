#include "robot_tplt.h"
#include "robot.h"
#include "sh.h"

static int
load_tplt(struct robot *self) {
    tplt_free(self->T);
#define TBLFILE(name) "./res/tbl/"#name".tbl"
    struct tplt_desc desc[] = {
        { TPLT_ROLE, sizeof(struct role_tplt), 1, TBLFILE(role), 0, TPLT_VIST_VEC32},
        { TPLT_XING, sizeof(struct xing_tplt), 1, TBLFILE(xing), 0, TPLT_VIST_VEC32},
        { TPLT_MING, sizeof(struct ming_tplt), 1, TBLFILE(ming), 0, TPLT_VIST_VEC32},
        { TPLT_TESHU, sizeof(struct teshu_tplt), 1, TBLFILE(teshu), 0, TPLT_VIST_VEC32},
    };
    self->T = tplt_create(desc, sizeof(desc)/sizeof(desc[0]));
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

int
robot_tplt_main(struct module *s) {
    struct robot *self = MODULE_SELF;
    if (!load_tplt(self)) {
        sh_info("reloadres tplt ok");
        return 0;
    } else {
        sh_error("reloadres tplt fail");
        return 1;
    }
}
