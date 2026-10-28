#pragma once
#include <stdbool.h>
#include <stdint.h>
#define APP_HERMES 1u
#define APP_CODEX 2u
#define APP_STOCKS 4u
typedef struct {bool night;uint16_t start,end;uint8_t apps;} preferences_t;
void preferences_init(void);
preferences_t preferences_get(void);
void preferences_set(preferences_t value);
void preferences_tick(void);
int preferences_brightness(void);
bool preferences_self_test(void);
