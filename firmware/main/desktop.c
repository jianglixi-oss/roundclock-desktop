#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "network.h"
#include "fonts.h"
#include "chinese_font.h"
#include "voice.h"
#include "codex.h"
#include "stocks.h"
#include "voice_config.h"
#include "panel_init.h"

#define W 360
#define STRIPE 20
#define WHITE 0xef5d
#define MUTED 0x7bcf
#define GOLD 0xfd8b
#define PURPLE 0xb29f
typedef enum {CLOCK,MENU,HERMES,SETUP,CODEX,STOCKS} Screen;
typedef struct {int x,y;} Tap;
static Screen screen=CLOCK;
static const char *names[]={"CLOCK","MENU","HERMES","SETUP","CODEX","STOCKS"};
static const char *TAG="desktop";
static esp_lcd_panel_handle_t panel;
static uint16_t *pixels;
static SemaphoreHandle_t transfer_done;
static QueueHandle_t taps;
static int stripe_y;
static struct tm shown_time;
static bool shown_valid;
static voice_view_t shown_voice;
static codex_view_t shown_codex;
static stocks_view_t shown_stocks;
static bool stock_settings=false;
static int64_t codex_notice_until=0;
static i2c_master_dev_handle_t touch;
static volatile bool touch_irq=false;
static bool touch_down=false;
static int64_t last_tap=0,last_activity=0;
static volatile bool capture_requested=false;

static bool serial_write_all(const void *data,size_t length)
{
    const uint8_t *p=data;
    while(length) {
        size_t chunk=length>512?512:length;
        int sent=usb_serial_jtag_write_bytes(p,chunk,pdMS_TO_TICKS(1000));
        if(sent<=0) return false;
        p+=sent;length-=sent;
    }
    return true;
}

static bool dma_done(esp_lcd_panel_io_handle_t io,esp_lcd_panel_io_event_data_t *event,void *ctx)
{
    (void)io; (void)event; (void)ctx;
    BaseType_t wake=pdFALSE; xSemaphoreGiveFromISR(transfer_done,&wake); return wake==pdTRUE;
}

static void pixel(int x,int y,uint16_t color,int alpha)
{
    if(x<0||x>=W||y<stripe_y||y>=stripe_y+STRIPE||alpha<=0) return;
    uint16_t *p=&pixels[(y-stripe_y)*W+x];
    if(alpha<15) {
        uint16_t bg=__builtin_bswap16(*p);
        int r=(((color>>11)&31)*alpha+((bg>>11)&31)*(15-alpha))/15;
        int g=(((color>>5)&63)*alpha+((bg>>5)&63)*(15-alpha))/15;
        int b=((color&31)*alpha+(bg&31)*(15-alpha))/15;
        color=(r<<11)|(g<<5)|b;
    }
    *p=__builtin_bswap16(color);
}

static void circle(int cx,int cy,int radius,uint16_t color)
{
    int top=cy-radius>stripe_y?cy-radius:stripe_y;
    int bottom=cy+radius<stripe_y+STRIPE-1?cy+radius:stripe_y+STRIPE-1;
    for(int y=top;y<=bottom;++y) for(int x=cx-radius;x<=cx+radius;++x) {
        int dx=x-cx,dy=y-cy;
        float distance=sqrtf((float)(dx*dx+dy*dy));
        int a=(int)((radius+0.5f-distance)*15);
        if(a>15)a=15;
        pixel(x,y,color,a);
    }
}

static void line(int x0,int y0,int x1,int y1,int thickness,uint16_t color)
{
    float dx=x1-x0,dy=y1-y0,length=dx*dx+dy*dy,r=thickness*0.5f;
    int left=(x0<x1?x0:x1)-(int)r-1,right=(x0>x1?x0:x1)+(int)r+1;
    int top=(y0<y1?y0:y1)-(int)r-1,bottom=(y0>y1?y0:y1)+(int)r+1;
    if(top<stripe_y)top=stripe_y;
    if(bottom>=stripe_y+STRIPE)bottom=stripe_y+STRIPE-1;
    for(int y=top;y<=bottom;++y)for(int x=left;x<=right;++x){
        float t=length>0?((x-x0)*dx+(y-y0)*dy)/length:0;
        if(t<0)t=0;
        if(t>1)t=1;
        float ex=x-x0-t*dx,ey=y-y0-t*dy;
        int a=(int)((r+0.5f-sqrtf(ex*ex+ey*ey))*15);
        if(a>15)a=15;
        pixel(x,y,color,a);
    }
}

static uint32_t next_code(const char **s)
{
    const uint8_t *p=(const uint8_t *)*s;uint32_t c=*p++;
    int count=c<0x80?0:c>=0xc2&&c<0xe0?1:c<0xf0?2:c<0xf5?3:-1;
    if(count<0){*s=(const char *)p;return '?';}
    if(count)c&=(1U<<(6-count))-1;
    for(int i=0;i<count;++i){if(!*p||(*p&0xc0)!=0x80){*s=(const char *)p;return '?';}c=(c<<6)|(*p++&63);}
    *s=(const char *)p;return c;
}

static const Glyph *find_glyph(const Glyph *glyphs,int count,uint32_t code)
{
    if(glyphs==chinese_glyphs){
        int left=0,right=count-1;
        while(left<=right){int mid=(left+right)/2;if(glyphs[mid].code==code)return &glyphs[mid];if(glyphs[mid].code<code)left=mid+1;else right=mid-1;}
        return code!='?'?find_glyph(chinese_glyphs,CHINESE_COUNT,'?'):NULL;
    }
    for(int i=0;i<count;++i) if(glyphs[i].code==code) return &glyphs[i];
    return NULL;
}

static void label(const char *text,int center,int y,int font,uint16_t color)
{
    const Glyph *glyphs=font==5?chinese_glyphs:font==4?setup_glyphs:font==3?dial_glyphs:font==2?big_glyphs:(font==1?label_glyphs:small_glyphs);
    const uint8_t *data=font==5?chinese_data:font==4?setup_data:font==3?dial_data:font==2?big_data:(font==1?label_data:small_data);
    int count=font==5?CHINESE_COUNT:font==4?SETUP_COUNT:font==3?DIAL_COUNT:font==2?BIG_COUNT:(font==1?LABEL_COUNT:SMALL_COUNT),width=0;
    const char *s=text;
    while(*s){const Glyph *g=find_glyph(glyphs,count,next_code(&s));if(g) width+=g->offset_w;}
    int left=center-width/2;s=text;
    while(*s) {
        const Glyph *g=find_glyph(glyphs,count,next_code(&s));if(!g) continue;
        int top=stripe_y>y?stripe_y-y:0;
        int bottom=stripe_y+STRIPE-y<g->h?stripe_y+STRIPE-y:g->h;
        for(int yy=top;yy<bottom;++yy) for(int xx=0;xx<g->w;++xx) {
            int i=yy*g->w+xx;uint8_t a=data[g->offset+i/2];
            pixel(left+xx,y+yy,color,(i&1)?(a&15):(a>>4));
        }
        left+=g->offset_w;
    }
}

static void wrapped(const char *text,int y,int max_width,int max_lines,uint16_t color)
{
    char row[160];int used=0,width=0,lines=0;
    while(*text&&lines<max_lines){
        const char *start=text;uint32_t code=next_code(&text);int bytes=text-start;
        const Glyph *g=find_glyph(chinese_glyphs,CHINESE_COUNT,code);int advance=g?g->offset_w:12;
        if(code=='\n'||(width+advance>max_width&&used)){
            row[used]=0;label(row,180,y+lines*26,5,color);++lines;used=width=0;
            if(lines>=max_lines)break;
            if(code=='\n')continue;
        }
        if(used+bytes>=(int)sizeof(row))break;
        memcpy(row+used,start,bytes);used+=bytes;width+=advance;
    }
    if(used&&lines<max_lines){row[used]=0;label(row,180,y+lines*26,5,color);}
}

static uint16_t color_mix(uint16_t a,uint16_t b,float t)
{
    int r=((a>>11)&31)*(1-t)+((b>>11)&31)*t;
    int g=((a>>5)&63)*(1-t)+((b>>5)&63)*t;
    int blue=(a&31)*(1-t)+(b&31)*t;
    return (r<<11)|(g<<5)|blue;
}

static void tile(int cx,int cy,uint16_t top_color,uint16_t bottom_color)
{
    for(int y=stripe_y;y<stripe_y+STRIPE;++y)for(int x=cx-43;x<=cx+43;++x){
        if(y<cy-43||y>cy+43)continue;
        float dx=fmaxf(abs(x-cx)-23,0),dy=fmaxf(abs(y-cy)-23,0);
        int a=(int)((20.5f-sqrtf(dx*dx+dy*dy))*15);if(a>15)a=15;
        pixel(x,y,color_mix(top_color,bottom_color,(y-cy+43)/86.0f),a);
    }
}

static void clock_icon(int x,int y)
{
    tile(x,y,0x18c3,0x0000);circle(x,y,34,0xffff);
    for(int i=0;i<12;++i){float a=i*6.2831853f/12;line(x+lroundf(sinf(a)*28),y-lroundf(cosf(a)*28),x+lroundf(sinf(a)*31),y-lroundf(cosf(a)*31),2,0x0000);}
    line(x,y,x-13,y-15,5,0x0000);line(x,y,x+20,y-13,3,0x0000);
    line(x,y+7,x,y-25,1,0xfba0);circle(x,y,3,0x0000);circle(x,y,1,0xfba0);
}

static void hermes_icon(int x,int y)
{
    tile(x,y,0xfd89,0xf9a5);
    line(x-15,y-19,x-15,y+20,6,0xffff);line(x+15,y-19,x+15,y+20,6,0xffff);
    line(x-15,y,x+15,y,5,0xffff);
    line(x-15,y-18,x-25,y-25,3,0xffff);line(x+15,y-18,x+25,y-25,3,0xffff);
    line(x-20,y-13,x-29,y-19,2,0xffff);line(x+20,y-13,x+29,y-19,2,0xffff);
}

static void microphone_icon(int x,int y,bool listening)
{
    tile(x,y,listening?0xfaa8:0x2d7f,listening?0xd906:0x0233);
    line(x,y-21,x,y+2,15,WHITE);
    int px=x-19,py=y-1;
    for(int i=1;i<=24;++i){
        float a=3.14159265f-i*3.14159265f/24;
        int nx=x+lroundf(cosf(a)*19),ny=y-1+lroundf(sinf(a)*19);
        line(px,py,nx,ny,3,WHITE);px=nx;py=ny;
    }
    line(x,y+18,x,y+28,3,WHITE);line(x-10,y+28,x+10,y+28,3,WHITE);
}

static void codex_icon(int x,int y)
{
    tile(x,y,0xf7be,0xbdf7);
    line(x-23,y-15,x-9,y,5,0x1082);line(x-9,y,x-23,y+15,5,0x1082);
    line(x+2,y+16,x+22,y+16,5,0x1082);
}

static void stock_price(const char *text,int y,uint16_t color)
{
    int width=0;const char *s=text;
    while(*s){const Glyph *g=find_glyph(label_glyphs,LABEL_COUNT,next_code(&s));if(g)width+=g->offset_w*2;}
    int left=180-width/2;s=text;
    while(*s){
        const Glyph *g=find_glyph(label_glyphs,LABEL_COUNT,next_code(&s));if(!g)continue;
        for(int yy=0;yy<g->h*2;++yy){
            if(y+yy<stripe_y||y+yy>=stripe_y+STRIPE)continue;
            for(int xx=0;xx<g->w*2;++xx){int i=(yy/2)*g->w+xx/2;uint8_t a=label_data[g->offset+i/2];a=i%2?a&15:a>>4;pixel(left+xx,y+yy,color,a);}
        }
        left+=g->offset_w*2;
    }
}

static void stocks_icon(int x,int y)
{
    tile(x,y,0x2124,0x0000);
    line(x-30,y+24,x-16,y+9,4,WHITE);line(x-16,y+9,x-5,y+14,4,WHITE);
    line(x-5,y+14,x+8,y-12,4,WHITE);line(x+8,y-12,x+20,y-2,4,WHITE);line(x+20,y-2,x+31,y-25,4,WHITE);
    line(x-30,y+30,x+31,y+30,1,0x632c);
}

static bool codex_banner_visible(codex_state_t state)
{
    if(state==CX_WORKING||state==CX_WAITING)return true;
    return (state==CX_COMPLETED||state==CX_ERROR)&&esp_timer_get_time()<codex_notice_until;
}

static void codex_pet(int x,int y)
{
    uint16_t accent=shown_codex.state==CX_ERROR?0xfa27:shown_codex.state==CX_WAITING?GOLD:0x5ef5;
    circle(x,y,29,0x18e4);line(x-17,y-18,x+17,y-18,5,accent);
    bool blink=(esp_timer_get_time()/250000)%16==0;
    if(shown_codex.state==CX_OFFLINE){line(x-16,y-2,x-7,y-2,3,MUTED);line(x+7,y-2,x+16,y-2,3,MUTED);}
    else{line(x-12,y-5,x-12,y+(blink?-5:2),5,accent);line(x+12,y-5,x+12,y+(blink?-5:2),5,accent);}
    line(x-7,y+13,x+7,y+13,2,accent);
    if(shown_codex.state==CX_WORKING){int dot=(esp_timer_get_time()/300000)%3;circle(x-10+dot*10,y+37,2,accent);}
    if(shown_codex.state==CX_COMPLETED){line(x+20,y+20,x+25,y+26,3,accent);line(x+25,y+26,x+36,y+13,3,accent);}
}

static void usage_bar(int used,int minutes,int64_t reset,int y)
{
    char text[80];const char *window=minutes>=10000?"每周":minutes==300?"5小时":"额度";
    if(used<0)snprintf(text,sizeof(text),"%s  用量未知",window);
    else snprintf(text,sizeof(text),"%s  已用 %d%%",window,used);
    label(text,180,y,5,WHITE);line(80,y+30,280,y+30,8,0x2104);
    if(used>0)line(80,y+30,80+2*(used>100?100:used),y+30,8,used>=90?0xfa27:0x4ddf);
    if(reset>0){time_t epoch=reset;struct tm t;localtime_r(&epoch,&t);snprintf(text,sizeof(text),"%02d/%02d %02d:%02d 重置",t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min);}
    else snprintf(text,sizeof(text),"等待电脑同步");
    label(text,180,y+43,5,MUTED);
}

static void analog_clock(void)
{
    const float tau=6.283185307f;
    for(int i=0;i<60;++i){
        float a=i*tau/60;int inner=i%5==0?155:162;
        line(180+lroundf(sinf(a)*inner),180-lroundf(cosf(a)*inner),
             180+lroundf(sinf(a)*172),180-lroundf(cosf(a)*172),2,i%5==0?0xa514:0x630c);
    }
    for(int n=1;n<=12;++n){
        float a=n*tau/12;char number[4];snprintf(number,sizeof(number),"%d",n);
        label(number,180+lroundf(sinf(a)*132),180-lroundf(cosf(a)*132)-20,3,0xffff);
    }
    if(!shown_valid){label("请校时",180,225,0,GOLD);return;}
    const char *week[]={"日","一","二","三","四","五","六"};
    char date[64];
    snprintf(date,sizeof(date),"%d月%d日 星期%s",shown_time.tm_mon+1,shown_time.tm_mday,week[shown_time.tm_wday]);
    label(date,180,107,0,0xbdf7);
    float seconds=shown_time.tm_sec;
    float minute=(shown_time.tm_min+seconds/60)*tau/60;
    float hour=((shown_time.tm_hour%12)+shown_time.tm_min/60.0f+seconds/3600)*tau/12;
    float second=seconds*tau/60;
    line(180,180,180+lroundf(sinf(hour)*94),180-lroundf(cosf(hour)*94),11,0xffff);
    line(180,180,180+lroundf(sinf(minute)*143),180-lroundf(cosf(minute)*143),9,0xffff);
    line(180-lroundf(sinf(second)*25),180+lroundf(cosf(second)*25),
         180+lroundf(sinf(second)*164),180-lroundf(cosf(second)*164),2,0xfa27);
    circle(180,180,8,0xffff);circle(180,180,5,0xfa27);circle(180,180,2,0x0000);
}

static void scene(void)
{
    if(screen==CLOCK) {
        analog_clock();
    } else if(screen==MENU) {
        if(codex_banner_visible(shown_codex.state))label(codex_state_label(shown_codex.state),180,29,5,0x5ef5);
        else label("应用",180,25,1,WHITE);
        clock_icon(120,112);hermes_icon(240,112);codex_icon(120,240);stocks_icon(240,240);
        label("时钟",120,165,5,WHITE);label("Hermes",240,166,0,WHITE);
        label("Codex",120,293,0,WHITE);label("股票",240,292,5,WHITE);
        label("轻触空白返回",180,329,0,MUTED);
    } else if(screen==STOCKS) {
        label("股票",180,23,5,WHITE);
        if(stock_settings){
            label("手机添加自选股",180,93,5,WHITE);
            label(STOCKS_SETTINGS_HOST,180,133,0,0x5ef5);label("/stocks",180,161,0,0x5ef5);
            label("设置码",180,202,5,MUTED);label(shown_stocks.pin[0]?shown_stocks.pin:"等待连接",180,232,1,WHITE);
            label("轻触返回行情",180,292,5,MUTED);
        }else if(!shown_stocks.count){
            label("暂无自选股",180,142,5,WHITE);label("轻触添加股票",180,197,5,0x5ef5);
            label("返回菜单",180,316,5,MUTED);
        }else{
            char text[96];label(shown_stocks.online?shown_stocks.market:"连接异常 · 缓存行情",180,51,5,MUTED);
            for(int row=0;row<5&&shown_stocks.index+row<shown_stocks.count;++row){
                stock_quote_t *q=&shown_stocks.quotes[shown_stocks.index+row];int y=79+row*40;
                char name[40];const char *end=q->name;for(int i=0;i<6&&*end;++i)next_code(&end);
                int bytes=end-q->name;if(bytes>(int)sizeof(name)-1)bytes=sizeof(name)-1;memcpy(name,q->name,bytes);name[bytes]=0;
                label(name,130,y,5,WHITE);label(q->code,113,y+20,0,MUTED);
                uint16_t color=q->change>0?0xfa27:q->change<0?0x4e0d:WHITE;
                if(q->valid)snprintf(text,sizeof(text),"%.2f",q->price);else snprintf(text,sizeof(text),"--");
                label(text,251,y+3,1,q->stale?MUTED:color);
            }
            if(shown_stocks.count>5){
                line(310,89,310,269,3,0x2104);int thumb=180*5/shown_stocks.count;
                int top=89+(180-thumb)*shown_stocks.index/(shown_stocks.count-5);
                line(310,top,310,top+thumb,4,0x9cd3);
            }
            snprintf(text,sizeof(text),"%s",shown_stocks.quotes[0].time);label(text,180,289,0,MUTED);
            label("返回",130,316,5,MUTED);label("添加/管理",222,316,5,0x5ef5);
        }
    } else if(screen==CODEX) {
        label("Codex",180,24,1,WHITE);codex_pet(180,86);
        label(codex_state_label(shown_codex.state),180,131,5,shown_codex.state==CX_ERROR?0xfa27:WHITE);
        usage_bar(shown_codex.primary_used,shown_codex.primary_minutes,shown_codex.primary_reset,165);
        usage_bar(shown_codex.secondary_used,shown_codex.secondary_minutes,shown_codex.secondary_reset,239);
        label(shown_codex.usage_stale?"返回菜单 · 用量待更新":"返回菜单",180,316,5,MUTED);
    } else if(screen==HERMES) {
        label("Hermes Agent",180,30,0,WHITE);
        wrapped(shown_voice.status,69,265,2,shown_voice.state==VOICE_ERROR?0xfa27:0x9e79);
        if(shown_voice.reply[0]){
            wrapped(shown_voice.transcript,110,250,1,MUTED);
            wrapped(shown_voice.reply,149,250,5,WHITE);
            label("自动滚动",91,281,5,MUTED);
            if(shown_voice.pages>1)label("重看",268,281,5,MUTED);
            circle(180,285,19,shown_voice.state==VOICE_SPEAKING?0x6acf:0x23ab);
            line(180,278,180,292,3,WHITE);
        }else{
            bool listening=shown_voice.state==VOICE_LISTENING;
            microphone_icon(180,170,listening);
            if(shown_voice.transcript[0])wrapped(shown_voice.transcript,226,250,2,WHITE);
            else{label("轻触开始对话",180,235,5,WHITE);label("说完可等待或轻触结束",180,265,5,MUTED);}
        }
        label("返回菜单",180,316,5,MUTED);
    } else {
        label("网络设置",180,49,1,WHITE);
        label(clock_ap_ssid,180,110,0,GOLD);
        label("免密码连接",180,151,4,WHITE);
        label("192.168.4.1",180,197,1,WHITE);
        label("轻触返回",180,280,0,MUTED);
    }
    if(screen==CLOCK&&codex_banner_visible(shown_codex.state)){
        line(92,287,268,287,28,0x18e4);
        label(codex_state_label(shown_codex.state),180,277,5,0x5ef5);
    }
}

static void render(void)
{
    bool capture=capture_requested;capture_requested=false;
    if(capture) {
        esp_log_level_set("*",ESP_LOG_NONE);
        capture=serial_write_all("\nFRAME 360 360\n",15);
    }
    time_t now=time(NULL);localtime_r(&now,&shown_time);
    shown_valid=clock_time_valid;
    if(screen==HERMES)voice_get_view(&shown_voice);
    codex_get_view(&shown_codex);
    stocks_get_view(&shown_stocks);
    for(stripe_y=0;stripe_y<W;stripe_y+=STRIPE) {
        memset(pixels,0,W*STRIPE*sizeof(uint16_t));scene();
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel,0,stripe_y,W,stripe_y+STRIPE,pixels));
        if(xSemaphoreTake(transfer_done,pdMS_TO_TICKS(2000))!=pdTRUE) {
            ESP_LOGE(TAG,"Display DMA timed out");abort();
        }
        if(capture) capture=serial_write_all(pixels,W*STRIPE*sizeof(uint16_t));
    }
    if(capture) serial_write_all("\nFRAMEEND\n",10);
    esp_log_level_set("*",ESP_LOG_INFO);
}

static void set_screen(Screen next)
{
    if(screen==HERMES&&next!=HERMES)voice_leave();
    if(screen!=HERMES&&next==HERMES)voice_enter();
    stocks_set_active(next==STOCKS);
    if(next==STOCKS)stock_settings=false;
    screen=next;last_activity=esp_timer_get_time();
    ESP_LOGI(TAG,"UI state=%s",names[screen]);
}

static void tapped(int x,int y)
{
    last_activity=esp_timer_get_time();
    ESP_LOGI(TAG,"Tap logical=(%d,%d)",x,y);
    if(screen==CLOCK) set_screen(y>=267&&codex_banner_visible(shown_codex.state)?CODEX:MENU);
    else if(screen==CODEX){if(y>=292)set_screen(MENU);}
    else if(screen==STOCKS){
        if(stock_settings){stock_settings=false;stocks_refresh();}
        else if(y>=300){if(x<175)set_screen(MENU);else stock_settings=true;}
        else if(!shown_stocks.count)stock_settings=true;
        else if(x>=297&&shown_stocks.count>5){int target=(y-79)*(shown_stocks.count-5)/200;stocks_next(target-shown_stocks.index);}
    }
    else if(screen==HERMES){if(y>=300||(y>=292&&x>=105&&x<=255))set_screen(MENU);else if(y>=250&&x>230)voice_next_page();else voice_toggle();}
    else if(screen==SETUP) set_screen(CLOCK);
    else if(x>=65&&x<=295&&y>=63&&y<=319) set_screen(y<191?(x<180?CLOCK:HERMES):(x<180?CODEX:STOCKS));
    else set_screen(CLOCK);
}

static void IRAM_ATTR touch_interrupt(void *arg){(void)arg;touch_irq=true;}

static void init_touch(void)
{
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_40,GPIO_MODE_OUTPUT));
    gpio_set_level(GPIO_NUM_40,0);vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(GPIO_NUM_40,1);vTaskDelay(pdMS_TO_TICKS(100));
    i2c_master_bus_config_t cfg={.i2c_port=I2C_NUM_1,.sda_io_num=7,.scl_io_num=8,
        .clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7,.flags.enable_internal_pullup=true};
    i2c_master_bus_handle_t bus;ESP_ERROR_CHECK(i2c_new_master_bus(&cfg,&bus));
    ESP_ERROR_CHECK(i2c_master_probe(bus,0x15,200));
    i2c_device_config_t dev={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=0x15,.scl_speed_hz=100000};
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus,&dev,&touch));
    uint8_t reg=0xa3,id=0;ESP_ERROR_CHECK(i2c_master_transmit_receive(touch,&reg,1,&id,1,100));
    ESP_LOGI(TAG,"CST816S chip ID=0x%02x",id);
    gpio_config_t irq={.pin_bit_mask=1ULL<<41,.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE,.intr_type=GPIO_INTR_NEGEDGE};
    ESP_ERROR_CHECK(gpio_config(&irq));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(GPIO_NUM_41,touch_interrupt,NULL));
    gpio_config_t boot={.pin_bit_mask=1ULL<<0,.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&boot));
}

static bool poll_touch(void)
{
    static int stock_drag_y=0;
    if(!touch_irq&&!touch_down) return false;
    bool was_irq=touch_irq;touch_irq=false;
    uint8_t reg=0x01,b[6]={0};
    if(i2c_master_transmit_receive(touch,&reg,1,b,sizeof(b),50)!=ESP_OK){touch_down=false;return false;}
    int count=b[1]&15,rawx=((b[2]&15)<<8)|b[3],rawy=((b[4]&15)<<8)|b[5];
    int event=b[2]>>6;bool pressed=count>0&&event!=1;
    bool start=pressed&&!touch_down;
    bool quick_tap=was_irq&&b[0]==5&&!touch_down&&!pressed;
    int64_t now=esp_timer_get_time();
    bool acted=false;
    if(start)stock_drag_y=W-1-rawx;
    if(screen==STOCKS&&!stock_settings&&pressed&&touch_down&&rawx<W&&rawy<W){
        int y=W-1-rawx,delta=stock_drag_y-y;
        if(abs(delta)>=28){stocks_next(delta>0?1:-1);stock_drag_y=y;acted=true;last_activity=now;}
    }
    if((start||quick_tap)&&now-last_tap>350000&&rawx<W&&rawy<W) {
        /* Raw Y follows left-to-right; physical bottom taps report low raw X. */
        int x=rawy,y=W-1-rawx;
        ESP_LOGI(TAG,"Touch raw=(%d,%d) gesture=%d event=%d",rawx,rawy,b[0],event);
        tapped(x,y);last_tap=now;acted=true;
    }
    touch_down=pressed;
    return acted;
}

static void serial_task(void *arg)
{
    (void)arg;char command[80];int length=0;
    while(true) {
        char c;int n=usb_serial_jtag_read_bytes(&c,1,pdMS_TO_TICKS(100));
        if(n<=0) continue;
        if(c=='\n'||c=='\r') {
            command[length]=0;
            if(strncmp(command,"TIME ",5)==0) clock_set_epoch(strtoll(command+5,NULL,10));
            else if(strncmp(command,"TAP ",4)==0) {
                Tap tap;if(sscanf(command+4,"%d %d",&tap.x,&tap.y)==2&&tap.x>=0&&tap.x<W&&tap.y>=0&&tap.y<W)
                    xQueueSend(taps,&tap,0);
            } else if(!strcmp(command,"CAPTURE")) capture_requested=true;
            else if(!strcmp(command,"SETUP")) {Tap tap={-1,-1};xQueueSend(taps,&tap,0);}
            else if(!strcmp(command,"SETUPINFO")) ESP_LOGI(TAG,"SETUP_SSID=%s SETUP_OPEN=1",clock_ap_ssid);
            else if(!strcmp(command,"SCAN")) {char *json=clock_scan_json();if(json){ESP_LOGI(TAG,"SCAN_RESULT=%s",json);free(json);}}
            else if(!strcmp(command,"HTTPTEST"))clock_http_scan_test();
            else if(!strcmp(command,"STATUS")) ESP_LOGI(TAG,"UI=%s TIME=%d WIFI=%d",names[screen],clock_time_valid,clock_wifi_connected);
            else voice_diagnostic(command);
            length=0;
        } else if(length<(int)sizeof(command)-1) command[length++]=c;
        else length=0;
    }
}

static void init_display(void)
{
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_48,GPIO_MODE_OUTPUT));gpio_set_level(GPIO_NUM_48,0);
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_15,GPIO_MODE_OUTPUT));gpio_set_level(GPIO_NUM_15,0);
    transfer_done=xSemaphoreCreateBinary();
    pixels=heap_caps_malloc(W*STRIPE*sizeof(uint16_t),MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    if(!transfer_done||!pixels) abort();
    const spi_bus_config_t bus={.sclk_io_num=9,.data0_io_num=11,.data1_io_num=12,
        .data2_io_num=13,.data3_io_num=14,.max_transfer_sz=W*STRIPE*sizeof(uint16_t)};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io_cfg={.cs_gpio_num=10,.dc_gpio_num=-1,.spi_mode=0,
        .pclk_hz=20000000,.trans_queue_depth=2,.on_color_trans_done=dma_done,
        .lcd_cmd_bits=32,.lcd_param_bits=8,.flags.quad_mode=true};
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,&io_cfg,&io));
    st77916_vendor_config_t vendor={.init_cmds=lcd_init_cmds,.init_cmds_size=sizeof(lcd_init_cmds)/sizeof(lcd_init_cmds[0]),.flags.use_qspi_interface=true};
    esp_lcd_panel_dev_config_t config={.reset_gpio_num=47,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,.bits_per_pixel=16,.vendor_config=&vendor};
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(io,&config,&panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel,true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel,true));ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel,true,false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,true));
}

void app_main(void)
{
    init_display();render();gpio_set_level(GPIO_NUM_15,1);
    init_touch();
    clock_network_start();
    taps=xQueueCreate(4,sizeof(Tap));if(!taps) abort();
    if(!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg={.tx_buffer_size=2048,.rx_buffer_size=256};
        ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    }
    voice_init();
    codex_init();
    stocks_init();
    if(xTaskCreate(serial_task,"serial",4096,NULL,3,NULL)!=pdPASS) abort();
    set_screen(CLOCK);int64_t boot_start=0;bool boot_acted=false;
    time_t last_second=0;uint32_t last_voice_revision=0,last_codex_revision=0,last_stocks_revision=0;
    codex_state_t previous_codex=CX_OFFLINE;int64_t previous_codex_at=0,last_codex_spoken=0;bool codex_seen=false;
    for(;;) {
        bool dirty=poll_touch();Tap tap;
        while(xQueueReceive(taps,&tap,0)==pdTRUE){if(tap.x<0)set_screen(SETUP);else tapped(tap.x,tap.y);dirty=true;}
        int64_t now=esp_timer_get_time();
        if(gpio_get_level(GPIO_NUM_0)==0) {
            if(!boot_start) boot_start=now;
            if(!boot_acted&&now-boot_start>2000000){set_screen(SETUP);dirty=true;boot_acted=true;}
        } else {boot_start=0;boot_acted=false;}
        if(screen!=CLOCK&&screen!=STOCKS&&screen!=CODEX&&!(screen==HERMES&&voice_session_active())&&now-last_activity>60000000){set_screen(CLOCK);dirty=true;}
        if(screen==STOCKS&&stocks_revision()!=last_stocks_revision){last_stocks_revision=stocks_revision();dirty=true;}
        if(screen==HERMES)voice_tick();
        if(screen==HERMES&&voice_revision()!=last_voice_revision){last_voice_revision=voice_revision();dirty=true;}
        if(codex_revision()!=last_codex_revision){
            last_codex_revision=codex_revision();codex_view_t next;codex_get_view(&next);
            if(next.state!=previous_codex||(next.state==CX_COMPLETED&&next.state_at!=previous_codex_at)){
                if(codex_seen&&next.state==CX_COMPLETED&&next.state_at>last_codex_spoken+5&&llabs((long long)time(NULL)-next.state_at)<30){
                    voice_notify_completion();last_codex_spoken=next.state_at;
                }
                if(next.state==CX_WORKING||next.state==CX_WAITING||next.state==CX_COMPLETED||next.state==CX_ERROR)codex_notice_until=now+10000000;
                previous_codex=next.state;previous_codex_at=next.state_at;
                ESP_LOGI(TAG,"Codex state=%d used=%d/%d",next.state,next.primary_used,next.secondary_used);
            }
            if(next.state!=CX_OFFLINE)codex_seen=true;
            if(screen==CODEX||screen==CLOCK||screen==MENU)dirty=true;
        }
        time_t second=time(NULL);
        if(second!=last_second){last_second=second;if(screen==CLOCK||screen==CODEX||now<codex_notice_until+1000000) dirty=true;}
        if(capture_requested) dirty=true;
        if(dirty) render();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
