#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {char name[64],code[16],time[24];float price,change,percent,high,low;bool valid,stale;} stock_quote_t;
typedef struct {stock_quote_t quotes[12];int count,index;char market[48],pin[12];bool online;} stocks_view_t;
void stocks_init(void);
void stocks_set_active(bool active);
void stocks_get_view(stocks_view_t *view);
void stocks_next(int direction);
void stocks_refresh(void);
uint32_t stocks_revision(void);
