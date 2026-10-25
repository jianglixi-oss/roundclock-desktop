#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "network.h"
#include "voice_config.h"
#include "stocks.h"
static SemaphoreHandle_t mutex;static stocks_view_t current;static volatile bool active=false;static volatile uint32_t revision=1;
static volatile bool refresh_requested=false;
static void string(cJSON *root,const char *key,char *target,size_t cap){cJSON *v=cJSON_GetObjectItemCaseSensitive(root,key);snprintf(target,cap,"%s",cJSON_IsString(v)?v->valuestring:"");}
static float number(cJSON *root,const char *key){cJSON *v=cJSON_GetObjectItemCaseSensitive(root,key);return cJSON_IsNumber(v)?v->valuedouble:0;}
void stocks_get_view(stocks_view_t *view){if(!mutex){*view=current;return;}xSemaphoreTake(mutex,portMAX_DELAY);*view=current;xSemaphoreGive(mutex);}
void stocks_set_active(bool value){active=value;if(value)refresh_requested=true;}
void stocks_refresh(void){refresh_requested=true;}
uint32_t stocks_revision(void){return revision;}
void stocks_next(int direction){xSemaphoreTake(mutex,portMAX_DELAY);int max=current.count>5?current.count-5:0;current.index+=direction;if(current.index<0)current.index=0;if(current.index>max)current.index=max;++revision;xSemaphoreGive(mutex);}
static void worker(void *arg){
    (void)arg;char authorization[160];const char *start=strchr(VOICE_AUTH_HEADER,':');start=start?start+1:"";while(*start==' ')++start;
    size_t n=strcspn(start,"\r\n");if(n>=sizeof(authorization))n=sizeof(authorization)-1;memcpy(authorization,start,n);authorization[n]=0;
    int ticks=0;
    for(;;){
        if(active&&clock_wifi_connected&&(ticks<=0||refresh_requested)){
            refresh_requested=false;
            esp_http_client_config_t cfg={.url=VOICE_HTTP_BASE_URL "/stocks/snapshot",.timeout_ms=9000,.buffer_size=2048};
            esp_http_client_handle_t client=esp_http_client_init(&cfg);char *data=malloc(8192);bool ok=false;
            if(client&&data){
                esp_http_client_set_header(client,"Authorization",authorization);
                if(esp_http_client_open(client,0)==ESP_OK){
                    esp_http_client_fetch_headers(client);int used=0,count;
                    while(used<8191&&(count=esp_http_client_read(client,data+used,8191-used))>0)used+=count;
                    data[used]=0;cJSON *root=esp_http_client_get_status_code(client)==200?cJSON_Parse(data):NULL;
                    if(root){
                        cJSON *items=cJSON_GetObjectItemCaseSensitive(root,"quotes");
                        if(cJSON_IsArray(items)){
                            xSemaphoreTake(mutex,portMAX_DELAY);current.count=cJSON_GetArraySize(items);if(current.count>12)current.count=12;
                            int max=current.count>5?current.count-5:0;if(current.index>max)current.index=max;
                            string(root,"market",current.market,sizeof(current.market));string(root,"pin",current.pin,sizeof(current.pin));
                            for(int i=0;i<current.count;++i){
                                cJSON *item=cJSON_GetArrayItem(items,i);stock_quote_t *q=&current.quotes[i];memset(q,0,sizeof(*q));
                                string(item,"name",q->name,sizeof(q->name));string(item,"code",q->code,sizeof(q->code));string(item,"time",q->time,sizeof(q->time));
                                q->price=number(item,"price");q->change=number(item,"change");q->percent=number(item,"percent");q->high=number(item,"high");q->low=number(item,"low");
                                q->valid=q->price>0;q->stale=!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(item,"stale"));
                            }
                            current.online=true;++revision;xSemaphoreGive(mutex);ok=true;
                        }
                        cJSON_Delete(root);
                    }
                    esp_http_client_close(client);
                }
            }
            if(client)esp_http_client_cleanup(client);
            free(data);
            if(!ok){xSemaphoreTake(mutex,portMAX_DELAY);current.online=false;++revision;xSemaphoreGive(mutex);}
            ticks=2;
        }
        if(!active)ticks=0;else if(ticks>0)--ticks;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
void stocks_init(void){mutex=xSemaphoreCreateMutex();configASSERT(mutex);configASSERT(xTaskCreate(worker,"stocks",6144,NULL,3,NULL)==pdPASS);}
