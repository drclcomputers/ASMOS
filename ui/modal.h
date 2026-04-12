#ifndef MODAL_H
#define MODAL_H

#include "lib/core.h"

typedef void (*modal_cb)(void);

typedef enum {
    MODAL_INFO,
    MODAL_WARNING,
    MODAL_ERROR,
    MODAL_CONFIRM,
} modal_type;

void modal_show(modal_type type, const char *title, const char *message, modal_cb on_confirm, modal_cb on_cancel);
bool modal_active(void);
void modal_update(void);
void modal_draw(void);

#endif
