#include <time.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "nvs.h"
#include "esp_log.h"
#include "network.h"
#include "preferences.h"
static preferences_t prefs={.night=false,.start=22*60,.end=7*60,.apps=7};
static int brightness=100;
static bool contains(int minute,int start,int end){return start==end?false:start<end?(minute>=start&&minute<end):(minute>=start||minute<end);}
static void save(void){
    nvs_handle_t handle;
    if(nvs_open("desktop",NVS_READWRITE,&handle)!=ESP_OK)return;
    uint8_t data[7]={1,prefs.night,prefs.apps,prefs.start&255,prefs.start>>8,prefs.end&255,prefs.end>>8};
    esp_err_t err=nvs_set_blob(handle,"preferences",data,sizeof(data));
    if(err==ESP_OK)err=nvs_commit(handle);
    nvs_close(handle);if(err!=ESP_OK)ESP_LOGE("preferences","Settings save failed: %s",esp_err_to_name(err));
}
preferences_t preferences_get(void){return prefs;}
void preferences_tick(void){
    time_t now=time(NULL);struct tm local;localtime_r(&now,&local);
    int next=prefs.night&&clock_time_valid&&contains(local.tm_hour*60+local.tm_min,prefs.start,prefs.end)?50:100;
    if(next!=brightness){
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,next==50?512:1023));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0));brightness=next;
        ESP_LOGI("preferences","Backlight=%d%%",brightness);
    }
}
void preferences_set(preferences_t value){
    if(value.start>=1440||value.end>=1440)return;
    value.apps&=7;
    if(value.night==prefs.night&&value.start==prefs.start&&value.end==prefs.end&&value.apps==prefs.apps)return;
    prefs=value;save();preferences_tick();
}
int preferences_brightness(void){return brightness;}
bool preferences_self_test(void){
    bool schedule=!contains(21*60+59,1320,420)&&contains(1320,1320,420)&&contains(0,1320,420)&&contains(419,1320,420)
        &&!contains(420,1320,420)&&!contains(12*60,1320,420)&&contains(600,480,1080)&&!contains(1080,480,1080)&&!contains(0,0,0);
    uint32_t before=ledc_get_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0);
    bool pwm=ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,512)==ESP_OK&&ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0)==ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(20));
    pwm=pwm&&ledc_get_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0)==512;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,before));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0));
    vTaskDelay(pdMS_TO_TICKS(20));
    return schedule&&pwm&&ledc_get_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0)==before;
}
void preferences_init(void){
    nvs_handle_t handle;
    if(nvs_open("desktop",NVS_READONLY,&handle)==ESP_OK){
        uint8_t data[7];size_t size=sizeof(data);
        if(nvs_get_blob(handle,"preferences",data,&size)==ESP_OK&&size==7&&data[0]==1){
            uint16_t start=data[3]|data[4]<<8,end=data[5]|data[6]<<8;
            if(start<1440&&end<1440)prefs=(preferences_t){.night=data[1]!=0,.apps=data[2]&7,.start=start,.end=end};
        }
        nvs_close(handle);
    }
    ledc_timer_config_t timer={.speed_mode=LEDC_LOW_SPEED_MODE,.timer_num=LEDC_TIMER_0,.duty_resolution=LEDC_TIMER_10_BIT,.freq_hz=5000,.clk_cfg=LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t channel={.gpio_num=15,.speed_mode=LEDC_LOW_SPEED_MODE,.channel=LEDC_CHANNEL_0,.timer_sel=LEDC_TIMER_0,.duty=1023,.hpoint=0};
    ESP_ERROR_CHECK(ledc_channel_config(&channel));preferences_tick();
}
