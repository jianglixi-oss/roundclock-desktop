#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "network.h"
#include "voice_config.h"
#include "codex.h"
static SemaphoreHandle_t mutex;
static codex_view_t current={.state=CX_OFFLINE,.primary_used=-1,.secondary_used=-1,.usage_stale=true};
static volatile uint32_t revision=1;
const char *codex_state_label(codex_state_t state){
    static const char *labels[]={"电脑离线","正在工作","等待你回复","任务已完成","遇到问题","待命中","状态待更新"};
    return labels[state>=CX_OFFLINE&&state<=CX_UNKNOWN?state:CX_UNKNOWN];
}
void codex_get_view(codex_view_t *view){if(!mutex){*view=current;return;}xSemaphoreTake(mutex,portMAX_DELAY);*view=current;xSemaphoreGive(mutex);}
uint32_t codex_revision(void){return revision;}
static int64_t number(cJSON *body,const char *name,int64_t fallback){cJSON *v=cJSON_GetObjectItemCaseSensitive(body,name);return cJSON_IsNumber(v)?(int64_t)v->valuedouble:fallback;}
static void worker(void *arg){
    (void)arg;char authorization[160];const char *start=strchr(VOICE_AUTH_HEADER,':');
    start=start?start+1:"";while(*start==' ')++start;
    size_t length=strcspn(start,"\r\n");if(length>=sizeof(authorization))length=sizeof(authorization)-1;
    memcpy(authorization,start,length);authorization[length]=0;
    for(;;){
        codex_view_t next;codex_get_view(&next);next.state=CX_OFFLINE;
        if(clock_wifi_connected){
            esp_http_client_config_t cfg={.url=VOICE_HTTP_BASE_URL "/codex/status",.timeout_ms=2000,.buffer_size=2048};
            esp_http_client_handle_t client=esp_http_client_init(&cfg);
            if(client){
                esp_http_client_set_header(client,"Authorization",authorization);
                if(esp_http_client_open(client,0)==ESP_OK){
                    esp_http_client_fetch_headers(client);char data[2048];int used=0,count;
                    while(used<(int)sizeof(data)-1&&(count=esp_http_client_read(client,data+used,sizeof(data)-1-used))>0)used+=count;
                    data[used]=0;
                    cJSON *root=esp_http_client_get_status_code(client)==200?cJSON_Parse(data):NULL;
                    if(root){
                        cJSON *state=cJSON_GetObjectItemCaseSensitive(root,"state");
                        static const char *names[]={"offline","working","waiting","completed","error","idle","unknown"};
                        if(cJSON_IsString(state))for(int i=0;i<7;++i)if(!strcmp(state->valuestring,names[i]))next.state=i;
                        cJSON *a=cJSON_GetObjectItemCaseSensitive(root,"primary"),*b=cJSON_GetObjectItemCaseSensitive(root,"secondary");
                        next.primary_used=number(a,"used",-1);next.secondary_used=number(b,"used",-1);
                        next.primary_reset=number(a,"reset",0);next.secondary_reset=number(b,"reset",0);
                        next.primary_minutes=number(a,"minutes",0);next.secondary_minutes=number(b,"minutes",0);
                        next.notification=number(root,"notification",0);next.active=number(root,"active",0);next.state_at=number(root,"state_at",0);
                        next.usage_stale=!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root,"usage_stale"));
                        cJSON_Delete(root);
                    }
                    esp_http_client_close(client);
                }
                esp_http_client_cleanup(client);
            }
        }
        if(clock_wifi_connected){
            wifi_ap_record_t ap;esp_netif_ip_info_t ip;
            esp_netif_t *netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if(netif&&esp_wifi_sta_get_ap_info(&ap)==ESP_OK&&esp_netif_get_ip_info(netif,&ip)==ESP_OK){
                cJSON *body=cJSON_CreateObject();char address[20];snprintf(address,sizeof(address),IPSTR,IP2STR(&ip.ip));
                cJSON_AddStringToObject(body,"ssid",(char*)ap.ssid);cJSON_AddStringToObject(body,"ip",address);cJSON_AddNumberToObject(body,"rssi",ap.rssi);
                char *json=cJSON_PrintUnformatted(body);cJSON_Delete(body);
                esp_http_client_config_t cfg={.url=VOICE_HTTP_BASE_URL "/settings/device",.timeout_ms=2000};
                esp_http_client_handle_t report=esp_http_client_init(&cfg);
                if(report&&json){esp_http_client_set_method(report,HTTP_METHOD_POST);esp_http_client_set_header(report,"Authorization",authorization);esp_http_client_set_header(report,"Content-Type","application/json");esp_http_client_set_post_field(report,json,strlen(json));esp_http_client_perform(report);}
                if(report){esp_http_client_cleanup(report);}
                free(json);
            }
        }
        xSemaphoreTake(mutex,portMAX_DELAY);
        if(memcmp(&next,&current,sizeof(next))){current=next;++revision;}
        xSemaphoreGive(mutex);vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
void codex_init(void){mutex=xSemaphoreCreateMutex();configASSERT(mutex);configASSERT(xTaskCreate(worker,"codex",6144,NULL,3,NULL)==pdPASS);}
