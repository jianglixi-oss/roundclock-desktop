#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum {CX_OFFLINE,CX_WORKING,CX_WAITING,CX_COMPLETED,CX_ERROR,CX_IDLE,CX_UNKNOWN} codex_state_t;
typedef struct {
    codex_state_t state;
    int primary_used,secondary_used,primary_minutes,secondary_minutes,active;
    int64_t primary_reset,secondary_reset,state_at;
    bool usage_stale;
} codex_view_t;
void codex_init(void);
void codex_get_view(codex_view_t *view);
uint32_t codex_revision(void);
const char *codex_state_label(codex_state_t state);
