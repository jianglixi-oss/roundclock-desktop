#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum {VOICE_OFF,VOICE_CONNECTING,VOICE_IDLE,VOICE_LISTENING,VOICE_THINKING,VOICE_SPEAKING,VOICE_ERROR} voice_state_t;
typedef struct {
    voice_state_t state;
    int level,page,pages;
    char status[96],transcript[256],reply[512];
} voice_view_t;
void voice_init(void);
void voice_enter(void);
void voice_leave(void);
void voice_toggle(void);
void voice_next_page(void);
void voice_tick(void);
void voice_notify_completion(void);
void voice_get_view(voice_view_t *view);
uint32_t voice_revision(void);
bool voice_session_active(void);
void voice_diagnostic(const char *command);
