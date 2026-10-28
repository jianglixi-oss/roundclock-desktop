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
#include "preferences.h"
#include "panel_init.h"

#define W 360
#define STRIPE stripe_height
static int stripe_height=90;
#define WHITE 0xef5d
#define MUTED 0x7bcf
#define GOLD 0xfd8b
#define PURPLE 0xb29f
typedef enum {CLOCK,MENU,HERMES,SETUP,CODEX,STOCKS,OPTIONS} Screen;
typedef struct {int x,y;} Tap;
static volatile Screen screen=CLOCK;
static const char *names[]={"CLOCK","MENU","HERMES","SETUP","CODEX","STOCKS","OPTIONS"};
static const char *TAG="desktop";
static esp_lcd_panel_handle_t panel;
static uint16_t *pixels;
static uint16_t *pixel_buffers[2];
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
static uint16_t *icon_bitmaps[5];
static uint16_t *icon_render_target=NULL;
static float menu_position=0;
static float menu_animation_from=0;
static int64_t menu_animation_started=0;
static int menu_target=0,options_page=0;
static int time_edit_row=0;
static bool menu_dragging=false;
static int selected_time_part[2]={0,0};
static int benchmark_left=0;
static int64_t benchmark_start=0,perf_total=0,perf_draw=0,perf_io=0;
static unsigned perf_frames=0;
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
    if(alpha<=0)return;
    uint16_t *p;
    if(icon_render_target){if(x<0||x>=100||y<0||y>=100)return;p=&icon_render_target[y*100+x];}
    else{if(x<0||x>=W||y<stripe_y||y>=stripe_y+STRIPE)return;p=&pixels[(y-stripe_y)*W+x];}
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

static void line_alpha(int x0,int y0,int x1,int y1,int thickness,uint16_t color,int opacity)
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
        pixel(x,y,color,a*opacity/15);
    }
}

static void line(int x0,int y0,int x1,int y1,int thickness,uint16_t color){line_alpha(x0,y0,x1,y1,thickness,color,15);}

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

static void settings_icon(int x,int y)
{
    tile(x,y,0x94b2,0x39e7);
    for(int i=0;i<8;++i){float a=i*6.2831853f/8;line(x+lroundf(sinf(a)*21),y+lroundf(cosf(a)*21),x+lroundf(sinf(a)*29),y+lroundf(cosf(a)*29),9,WHITE);}
    circle(x,y,24,WHITE);circle(x,y,13,0x5aeb);circle(x,y,6,WHITE);
}

static void init_icon_cache(void)
{
    void (*draw[])(int,int)={clock_icon,hermes_icon,codex_icon,stocks_icon,settings_icon};
    for(int i=0;i<5;++i){
        icon_bitmaps[i]=heap_caps_calloc(100*100,sizeof(uint16_t),MALLOC_CAP_SPIRAM);if(!icon_bitmaps[i])abort();
        icon_render_target=icon_bitmaps[i];
        for(stripe_y=0;stripe_y<W;stripe_y+=STRIPE)draw[i](50,50);
    }
    icon_render_target=NULL;stripe_y=0;
}

static int menu_apps(Screen *apps)
{
    preferences_t p=preferences_get();int count=0;apps[count++]=CLOCK;
    if(p.apps&APP_HERMES)apps[count++]=HERMES;
    if(p.apps&APP_CODEX)apps[count++]=CODEX;
    if(p.apps&APP_STOCKS)apps[count++]=STOCKS;
    apps[count++]=OPTIONS;return count;
}

static void menu_move(int step)
{
    Screen apps[5];int count=menu_apps(apps);int previous=menu_target;menu_target+=step;
    if(menu_target<0)menu_target=0;
    if(menu_target>=count)menu_target=count-1;
    if(menu_target!=previous||fabsf(menu_target-menu_position)>0.005f){menu_animation_from=menu_position;menu_animation_started=esp_timer_get_time();}
}

static void scaled_icon(int index,int cx,int cy,float scale)
{
    int size=lroundf(100*scale),left=cx-size/2,top=cy-size/2;
    int step=(100<<16)/size,begin=left<0?0:left,end=left+size>W?W:left+size;
    for(int y=stripe_y;y<stripe_y+STRIPE;++y){
        if(y<top||y>=top+size)continue;
        int sy=((y-top)*step)>>16;uint16_t *row=pixels+(y-stripe_y)*W;
        for(int x=begin;x<end;++x)row[x]=icon_bitmaps[index][sy*100+(((x-left)*step)>>16)];
    }
}

static void draw_carousel(void)
{
    Screen apps[5];int count=menu_apps(apps);const char *labels[]={"时钟","Hermes Agent","Codex","股票","设置"};
    label("应用",180,36,1,WHITE);
    for(int i=0;i<count;++i){
        float distance=fabsf(i-menu_position);if(distance>1.9f)continue;
        float scale=1.4f-fminf(distance,1.0f)*0.65f;int x=180+lroundf((i-menu_position)*136);
        Screen app=apps[i];int icon=app==CLOCK?0:app==HERMES?1:app==CODEX?2:app==STOCKS?3:4;
        scaled_icon(icon,x,171,scale);
    }
    int selected=menu_target;if(selected>=count)selected=count-1;
    Screen current=apps[selected];int index=current==CLOCK?0:current==HERMES?1:current==CODEX?2:current==STOCKS?3:4;
    label(labels[index],180,254,5,WHITE);
    for(int i=0;i<count;++i)circle(180+(i-(count-1)/2.0f)*14,293,i==selected?4:2,i==selected?WHITE:MUTED);
    label("左右滑动 · 点击打开",180,316,5,MUTED);
}

static void toggle_switch(int x,int y,bool enabled)
{
    line(x-13,y,x+13,y,23,enabled?0x35b0:0x39e7);circle(x+(enabled?13:-13),y,9,WHITE);
}

static void draw_options(void)
{
    preferences_t p=preferences_get();char text[64];label(options_page==2?"应用管理":options_page==1?"夜间时段":options_page==3?"夜间模式":options_page==4?"网页配置":"设置",180,33,5,WHITE);
    if(options_page==0){
        line(89,112,271,112,56,0x18c3);label("夜间模式",144,94,5,WHITE);label(p.night?"已开启":"已关闭",144,118,5,MUTED);label(">",269,104,1,WHITE);
        line(89,197,271,197,56,0x18c3);label("应用管理",166,187,5,WHITE);label(">",269,189,1,WHITE);
        label("网页配置",170,260,5,MUTED);label(">",269,253,1,MUTED);
    }else if(options_page==4){
        label(STOCKS_SETTINGS_HOST,180,105,5,WHITE);label("/settings",180,139,5,WHITE);
        label("手机连接家中Wi-Fi",180,188,5,MUTED);label("无需验证",180,220,5,MUTED);
        label("设备Wi-Fi配网",180,270,5,WHITE);
    }else if(options_page==2){
        label("时钟始终启用",180,79,5,MUTED);
        label("Hermes Agent",135,122,5,WHITE);toggle_switch(267,134,p.apps&APP_HERMES);
        line(72,166,288,166,1,0x2104);
        label("Codex",135,188,5,WHITE);toggle_switch(267,200,p.apps&APP_CODEX);
        line(72,232,288,232,1,0x2104);
        label("股票",135,254,5,WHITE);toggle_switch(267,266,p.apps&APP_STOCKS);
    }else if(options_page==3){
        label("启用夜间",132,94,5,WHITE);toggle_switch(267,106,p.night);
        label(p.night?"已开启":"已关闭",180,133,5,MUTED);
        line(87,212,273,212,70,0x18c3);label("生效时段",180,179,5,MUTED);
        snprintf(text,sizeof(text),"%02d:%02d - %02d:%02d",p.start/60,p.start%60,p.end/60,p.end%60);label(text,180,211,1,WHITE);
        label("夜间亮度50%",180,270,5,MUTED);
    }else if(options_page==1){
        line(92,91,144,91,36,time_edit_row==0?0x2295:0x2104);line(216,91,268,91,36,time_edit_row==1?0x2295:0x2104);
        label("开始",118,81,5,WHITE);label("结束",242,81,5,WHITE);
        int value=time_edit_row?p.end:p.start,part=selected_time_part[time_edit_row];
        snprintf(text,sizeof(text),"%02d:%02d",value/60,value%60);stock_price(text,131,WHITE);
        label("小时",138,204,5,part?MUTED:0x5ef5);label("分钟",222,204,5,part?0x5ef5:MUTED);
        line(part?201:117,199,part?243:159,199,3,0x5ef5);
        line(78,261,112,261,44,0x3186);line(248,261,282,261,44,0x3186);
        label("-",95,245,1,WHITE);label("+",265,245,1,WHITE);
    }
    label(options_page?"返回设置":"返回菜单",180,316,5,MUTED);
}

static bool codex_banner_visible(codex_state_t state)
{
    if(!(preferences_get().apps&APP_CODEX))return false;
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
        draw_carousel();
    } else if(screen==OPTIONS){
        draw_options();
    } else if(screen==STOCKS) {
        label("股票",180,23,5,WHITE);
        if(stock_settings){
            label("手机添加自选股",180,93,5,WHITE);
            label(STOCKS_SETTINGS_HOST,180,133,0,0x5ef5);label("/settings#stocks",180,161,0,0x5ef5);
            label("无需验证",180,202,5,MUTED);
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
        line_alpha(92,273,268,273,28,0xffff,3);
        label(codex_state_label(shown_codex.state),180,263,5,0x5ef5);
    }
}

static void render(void)
{
    static Screen last_rendered=CLOCK;
    int64_t perf_started=esp_timer_get_time(),draw_us=0,io_us=0;
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
    bool partial=screen==MENU&&last_rendered==MENU&&!capture;
    int begin=partial?80:0,end=partial?320:W,buffer=0;bool in_flight=false;
    for(stripe_y=begin;stripe_y<end;stripe_y+=STRIPE) {
        pixels=pixel_buffers[buffer];
        int64_t stage=esp_timer_get_time();
        memset(pixels,0,W*STRIPE*sizeof(uint16_t));scene();
        draw_us+=esp_timer_get_time()-stage;stage=esp_timer_get_time();
        if(in_flight&&xSemaphoreTake(transfer_done,pdMS_TO_TICKS(2000))!=pdTRUE) {
            ESP_LOGE(TAG,"Display DMA timed out");abort();
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel,0,stripe_y,W,stripe_y+STRIPE,pixels));in_flight=true;
        io_us+=esp_timer_get_time()-stage;
        if(capture) capture=serial_write_all(pixels,W*STRIPE*sizeof(uint16_t));
        buffer^=1;
    }
    int64_t final_wait=esp_timer_get_time();
    if(in_flight&&xSemaphoreTake(transfer_done,pdMS_TO_TICKS(2000))!=pdTRUE){ESP_LOGE(TAG,"Final display DMA timed out");abort();}
    io_us+=esp_timer_get_time()-final_wait;last_rendered=screen;
    if(capture) serial_write_all("\nFRAMEEND\n",10);
    esp_log_level_set("*",ESP_LOG_INFO);
    if(screen==MENU&&!capture){
        ++perf_frames;perf_total+=esp_timer_get_time()-perf_started;perf_draw+=draw_us;perf_io+=io_us;
        if(benchmark_left>0&&--benchmark_left==0)ESP_LOGI(TAG,"BENCH frames=%u frame_ms=%.2f draw_ms=%.2f io_ms=%.2f fps=%.2f",perf_frames,perf_total/(1000.0*perf_frames),perf_draw/(1000.0*perf_frames),perf_io/(1000.0*perf_frames),perf_frames*1000000.0/(esp_timer_get_time()-benchmark_start));
    }
}

static void set_screen(Screen next)
{
    Screen previous=screen;
    if(screen==HERMES&&next!=HERMES)voice_leave();
    if(screen!=HERMES&&next==HERMES)voice_enter();
    stocks_set_active(next==STOCKS);
    if(next==STOCKS)stock_settings=false;
    if(next==OPTIONS)options_page=0;
    if(next==MENU){
        Screen apps[5];int count=menu_apps(apps);menu_target=0;
        for(int i=0;i<count;++i)if(apps[i]==previous)menu_target=i;
        menu_position=menu_target;menu_animation_from=menu_target;menu_dragging=false;
    }
    screen=next;last_activity=esp_timer_get_time();
    ESP_LOGI(TAG,"UI state=%s",names[screen]);
}

static void tapped(int x,int y)
{
    last_activity=esp_timer_get_time();
    ESP_LOGI(TAG,"Tap logical=(%d,%d)",x,y);
    if(screen==CLOCK) set_screen(y>=251&&y<=295&&codex_banner_visible(shown_codex.state)?CODEX:MENU);
    else if(screen==MENU){
        if(y>=90&&y<=245){
            if(x<105)menu_move(-1);
            else if(x>255)menu_move(1);
            else{Screen apps[5];int count=menu_apps(apps);if(menu_target<count)set_screen(apps[menu_target]);}
        }else if(y>335)set_screen(CLOCK);
    }
    else if(screen==OPTIONS){
        preferences_t p=preferences_get();
        if(y>=302){if(options_page)options_page=options_page==1?3:0;else set_screen(MENU);}
        else if(options_page==0){
            if(x>=58&&x<=302&&y>=82&&y<=145)options_page=3;
            else if(x>=58&&x<=302&&y>=166&&y<=229)options_page=2;
            else if(x>=90&&x<=302&&y>=245&&y<=286)options_page=4;
        }else if(options_page==4){if(y>=245&&y<=290)set_screen(SETUP);
        }else if(options_page==2){
            if(x>=228&&x<=306&&y>=111&&y<=157)p.apps^=APP_HERMES;
            else if(x>=228&&x<=306&&y>=177&&y<=223)p.apps^=APP_CODEX;
            else if(x>=228&&x<=306&&y>=243&&y<=289)p.apps^=APP_STOCKS;
            preferences_set(p);
        }else if(options_page==3){
            if(x>=228&&x<=306&&y>=82&&y<=130){p.night=!p.night;preferences_set(p);}
            else if(x>=55&&x<=305&&y>=169&&y<=250)options_page=1;
        }else if(options_page==1){
            if(y>=68&&y<=114){if(x>=70&&x<=166)time_edit_row=0;else if(x>=194&&x<=290)time_edit_row=1;}
            else if(y>=123&&y<=228){if(x>=100&&x<=173)selected_time_part[time_edit_row]=0;else if(x>=187&&x<=262)selected_time_part[time_edit_row]=1;}
            else if(y>=235&&y<=288&&((x>=55&&x<=135)||(x>=225&&x<=305))){
                int value=time_edit_row?p.end:p.start,delta=selected_time_part[time_edit_row]?1:60;
                value=(value+(x<180?-delta:delta)+1440)%1440;
                if(time_edit_row)p.end=value;else p.start=value;
                preferences_set(p);
            }
        }
    }
    else if(screen==CODEX){if(y>=292)set_screen(MENU);}
    else if(screen==STOCKS){
        if(stock_settings){stock_settings=false;stocks_refresh();}
        else if(y>=300){if(x<175)set_screen(MENU);else stock_settings=true;}
        else if(!shown_stocks.count)stock_settings=true;
        else if(x>=297&&shown_stocks.count>5){int target=(y-79)*(shown_stocks.count-5)/200;stocks_next(target-shown_stocks.index);}
    }
    else if(screen==HERMES){if(y>=300||(y>=292&&x>=105&&x<=255))set_screen(MENU);else if(y>=250&&x>230)voice_next_page();else voice_toggle();}
    else if(screen==SETUP) set_screen(CLOCK);
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
    static bool menu_gesture=false;
    static int menu_start_x=0,menu_start_y=0,menu_last_x=0,menu_last_y=0,menu_start_index=0;
    static bool options_gesture=false,options_moved=false;
    static int options_start_x=0,options_start_y=0;
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
    if(screen==OPTIONS&&start&&rawx<W&&rawy<W){options_gesture=true;options_moved=false;options_start_x=rawy;options_start_y=W-1-rawx;}
    if(options_gesture&&screen==OPTIONS&&pressed&&rawx<W&&rawy<W){
        if(abs(rawy-options_start_x)>18||abs(W-1-rawx-options_start_y)>18)options_moved=true;
    }
    if(options_gesture&&(!pressed||screen!=OPTIONS)){
        options_gesture=false;
        if(screen==OPTIONS&&!options_moved&&now-last_tap>150000){tapped(options_start_x,options_start_y);last_tap=now;acted=true;}
    }
    if(screen==MENU&&start&&rawx<W&&rawy<W){
        menu_gesture=true;menu_dragging=true;menu_start_x=menu_last_x=rawy;menu_start_y=menu_last_y=W-1-rawx;menu_start_index=menu_target;
    }
    if(menu_gesture&&screen==MENU&&pressed&&rawx<W&&rawy<W){
        Screen apps[5];int count=menu_apps(apps);bool moved=menu_last_x!=rawy||menu_last_y!=W-1-rawx;menu_last_x=rawy;menu_last_y=W-1-rawx;
        menu_position=fmaxf(0,fminf(count-1,menu_start_index-(menu_last_x-menu_start_x)/136.0f));if(moved)acted=true;last_activity=now;
    }
    if(menu_gesture&&(!pressed||screen!=MENU)){
        menu_gesture=false;menu_dragging=false;
        if(screen==MENU){
            int delta=menu_last_x-menu_start_x;
            if(abs(delta)>24&&abs(delta)>abs(menu_last_y-menu_start_y)){
                menu_target=menu_start_index;int steps=lroundf(-delta/136.0f);if(!steps)steps=delta<0?1:-1;menu_move(steps);
            }else tapped(menu_start_x,menu_start_y);
            last_tap=now;acted=true;
        }
    }
    if(start)stock_drag_y=W-1-rawx;
    if(screen==STOCKS&&!stock_settings&&pressed&&touch_down&&rawx<W&&rawy<W){
        int y=W-1-rawx,delta=stock_drag_y-y;
        if(abs(delta)>=28){stocks_next(delta>0?1:-1);stock_drag_y=y;acted=true;last_activity=now;}
    }
    if((start||quick_tap)&&!menu_gesture&&!options_gesture&&screen!=MENU&&screen!=OPTIONS&&now-last_tap>350000&&rawx<W&&rawy<W) {
        /* Raw Y follows left-to-right; physical bottom taps report low raw X. */
        int x=rawy,y=W-1-rawx;
        ESP_LOGI(TAG,"Touch raw=(%d,%d) gesture=%d event=%d",rawx,rawy,b[0],event);
        tapped(x,y);last_tap=now;acted=true;
    }
    else if(quick_tap&&!menu_gesture&&screen==MENU&&now-last_tap>350000&&rawx<W&&rawy<W){tapped(rawy,W-1-rawx);last_tap=now;acted=true;}
    else if(quick_tap&&!options_gesture&&screen==OPTIONS&&now-last_tap>150000&&rawx<W&&rawy<W){tapped(rawy,W-1-rawx);last_tap=now;acted=true;}
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
            else if(!strcmp(command,"PREFS")){preferences_t p=preferences_get();ESP_LOGI(TAG,"PREFS night=%d start=%u end=%u apps=%u brightness=%d",p.night,p.start,p.end,p.apps,preferences_brightness());}
            else if(!strcmp(command,"SETTINGSTEST"))ESP_LOGI(TAG,"SETTINGSTEST=%s",preferences_self_test()?"PASS":"FAIL");
            else if(!strcmp(command,"BENCH")){Tap tap={-5,0};xQueueSend(taps,&tap,0);}
            else if(!strcmp(command,"MENU NEXT")||!strcmp(command,"MENU PREV")){Tap tap={!strcmp(command,"MENU NEXT")?-2:-3,0};xQueueSend(taps,&tap,0);}
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
    const int heights[]={90,60,20};
    for(int i=0;i<3;++i){
        stripe_height=heights[i];
        pixel_buffers[0]=heap_caps_malloc(W*STRIPE*sizeof(uint16_t),MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
        pixel_buffers[1]=heap_caps_malloc(W*STRIPE*sizeof(uint16_t),MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
        if(pixel_buffers[0]&&pixel_buffers[1]&&(i==2||heap_caps_get_free_size(MALLOC_CAP_INTERNAL)>=110*1024))break;
        free(pixel_buffers[0]);free(pixel_buffers[1]);pixel_buffers[0]=pixel_buffers[1]=NULL;
    }
    pixels=pixel_buffers[0];if(!transfer_done||!pixel_buffers[0]||!pixel_buffers[1])abort();
    ESP_LOGI(TAG,"Display stripe=%d rows internal_free=%u",STRIPE,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    const spi_bus_config_t bus={.sclk_io_num=9,.data0_io_num=11,.data1_io_num=12,
        .data2_io_num=13,.data3_io_num=14,.max_transfer_sz=W*STRIPE*sizeof(uint16_t)};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io_cfg={.cs_gpio_num=10,.dc_gpio_num=-1,.spi_mode=0,
        .pclk_hz=40000000,.trans_queue_depth=2,.on_color_trans_done=dma_done,
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

static void desktop_task(void *arg)
{
    (void)arg;
    init_display();render();gpio_set_level(GPIO_NUM_15,1);
    init_touch();
    clock_network_start();
    preferences_init();init_icon_cache();
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
        while(xQueueReceive(taps,&tap,0)==pdTRUE){if(tap.x==-5){if(screen==MENU){perf_frames=0;perf_total=perf_draw=perf_io=0;benchmark_left=24;benchmark_start=esp_timer_get_time();}}else if(tap.x==-2||tap.x==-3){if(screen==MENU)menu_move(tap.x==-2?1:-1);}else if(tap.x<0)set_screen(SETUP);else tapped(tap.x,tap.y);dirty=true;}
        int64_t now=esp_timer_get_time();
        static int64_t menu_frame_at=0;
        if(screen==MENU&&!menu_dragging&&fabsf(menu_target-menu_position)>0.005f&&now-menu_frame_at>=30000){
            float t=fminf(1.0f,(now-menu_animation_started)/220000.0f),ease=t*t*(3-2*t);
            menu_position=menu_animation_from+(menu_target-menu_animation_from)*ease;menu_frame_at=now;dirty=true;
            if(t>=1||fabsf(menu_target-menu_position)<0.005f)menu_position=menu_target;
        }
        if(!(preferences_get().apps&APP_CODEX))codex_seen=false;
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
                if((preferences_get().apps&APP_CODEX)&&codex_seen&&next.state==CX_COMPLETED&&next.state_at>last_codex_spoken+5&&llabs((long long)time(NULL)-next.state_at)<30){
                    voice_notify_completion_kind(next.notification);last_codex_spoken=next.state_at;
                }
                if(next.state==CX_WORKING||next.state==CX_WAITING||next.state==CX_COMPLETED||next.state==CX_ERROR)codex_notice_until=now+10000000;
                previous_codex=next.state;previous_codex_at=next.state_at;
                ESP_LOGI(TAG,"Codex state=%d used=%d/%d",next.state,next.primary_used,next.secondary_used);
            }
            if((preferences_get().apps&APP_CODEX)&&next.state!=CX_OFFLINE)codex_seen=true;
            if(screen==CODEX||screen==CLOCK||screen==MENU)dirty=true;
        }
        time_t second=time(NULL);
        if(second!=last_second){last_second=second;preferences_tick();if(screen==CLOCK||screen==CODEX||now<codex_notice_until+1000000) dirty=true;}
        if(capture_requested) dirty=true;
        if(benchmark_left>0&&screen==MENU)dirty=true;
        if(dirty) render();
        vTaskDelay(screen==MENU?1:pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    if(xTaskCreatePinnedToCore(desktop_task,"desktop_ui",8192,NULL,4,NULL,1)!=pdPASS)abort();
}
