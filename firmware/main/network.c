#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "network.h"

volatile bool clock_time_valid=false;
volatile bool clock_wifi_connected=false;
char clock_ap_ssid[32];
static bool have_station=false;
static volatile bool scanning=false;
static SemaphoreHandle_t scan_lock;
static const char *TAG="clock_net";

bool clock_set_epoch(long long seconds)
{
    if (seconds<1735689600LL || seconds>4102444799LL) return false;
    struct timeval tv={.tv_sec=(time_t)seconds,.tv_usec=0};
    if (settimeofday(&tv,NULL)!=0) return false;
    clock_time_valid=true;
    ESP_LOGI(TAG,"Time synchronized: %lld",seconds);
    return true;
}

static void ntp_sync(struct timeval *tv)
{
    clock_time_valid=tv->tv_sec>=1735689600;
    ESP_LOGI(TAG,"NTP synchronized");
}

static void wifi_event(void *arg,esp_event_base_t base,int32_t id,void *data)
{
    (void)arg;
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_START && have_station) esp_wifi_connect();
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        clock_wifi_connected=false;
        if(have_station&&!scanning) esp_wifi_connect();
    }
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        clock_wifi_connected=true;
        ip_event_got_ip_t *event=data;
        ESP_LOGI(TAG,"Station address: " IPSTR,IP2STR(&event->ip_info.ip));
        esp_netif_sntp_start();
    }
}

extern const unsigned char setup_html_start[] asm("_binary_setup_html_start");
extern const unsigned char setup_html_end[] asm("_binary_setup_html_end");

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_send(req,(const char *)setup_html_start,setup_html_end-setup_html_start-1);
}

static esp_err_t reply(httpd_req_t *req,const char *status,const char *body)
{
    httpd_resp_set_status(req,status);
    httpd_resp_set_type(req,"application/json");
    return httpd_resp_send(req,body,HTTPD_RESP_USE_STRLEN);
}

/* Configuration writes are only accepted from clients attached to the setup AP. */
static bool from_setup_ap(httpd_req_t *req)
{
    struct sockaddr_storage peer={0};socklen_t len=sizeof(peer);uint32_t ipv4;
    if(getpeername(httpd_req_to_sockfd(req),(struct sockaddr *)&peer,&len)!=0)return false;
    if(peer.ss_family==AF_INET){
        ipv4=((struct sockaddr_in *)&peer)->sin_addr.s_addr;
    }
#if CONFIG_LWIP_IPV6
    else if(peer.ss_family==AF_INET6){
        const uint8_t *address=((struct sockaddr_in6 *)&peer)->sin6_addr.s6_addr;
        const uint8_t mapped_prefix[12]={0,0,0,0,0,0,0,0,0,0,0xff,0xff};
        if(memcmp(address,mapped_prefix,sizeof(mapped_prefix))!=0)return false;
        memcpy(&ipv4,address+12,sizeof(ipv4));
    }
#endif
    else return false;
    return (ntohl(ipv4)&0xffffff00UL)==0xc0a80400UL;
}

char *clock_scan_json(void)
{
    if(!scan_lock||xSemaphoreTake(scan_lock,0)!=pdTRUE) return strdup("{\"error\":\"正在扫描，请稍后重试\"}");
    scanning=true;
    if(have_station&&!clock_wifi_connected){esp_wifi_disconnect();vTaskDelay(pdMS_TO_TICKS(150));}
    wifi_scan_config_t config={.show_hidden=false,.scan_type=WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active={.min=40,.max=90}};
    esp_err_t err=esp_wifi_scan_start(&config,true);
    uint16_t count=24;
    wifi_ap_record_t *records=calloc(count,sizeof(wifi_ap_record_t));
    if(err==ESP_OK&&records)err=esp_wifi_scan_get_ap_records(&count,records);
    else {esp_wifi_clear_ap_list();if(!records)err=ESP_ERR_NO_MEM;}
    scanning=false;
    xSemaphoreGive(scan_lock);
    if(have_station&&!clock_wifi_connected)esp_wifi_connect();
    if(err!=ESP_OK){free(records);ESP_LOGW(TAG,"Wi-Fi scan: %s",esp_err_to_name(err));return strdup("{\"error\":\"扫描失败，请重试\"}");}
    cJSON *body=cJSON_CreateObject(),*networks=cJSON_CreateArray();
    if(!body||!networks){cJSON_Delete(body);cJSON_Delete(networks);free(records);return NULL;}
    cJSON_AddItemToObject(body,"networks",networks);
    for(int i=0;i<count;++i){
        char ssid[33];memcpy(ssid,records[i].ssid,32);ssid[32]=0;
        if(!ssid[0])continue;
        bool duplicate=false;
        for(int j=0;j<i;++j)if(!strncmp((const char *)records[j].ssid,ssid,32)){duplicate=true;break;}
        if(duplicate)continue;
        cJSON *item=cJSON_CreateObject();if(!item)continue;
        cJSON_AddStringToObject(item,"ssid",ssid);
        cJSON_AddNumberToObject(item,"rssi",records[i].rssi);
        cJSON_AddBoolToObject(item,"secure",records[i].authmode!=WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(networks,item);
    }
    free(records);
    char *result=cJSON_PrintUnformatted(body);cJSON_Delete(body);
    ESP_LOGI(TAG,"Wi-Fi scan complete");return result;
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if(!from_setup_ap(req))return reply(req,"403 Forbidden","{\"error\":\"请先连接设备热点\"}");
    ESP_LOGI(TAG,"HTTP Wi-Fi scan request accepted");
    char *json=clock_scan_json();
    if(!json)return reply(req,"503 Service Unavailable","{\"error\":\"暂时无法扫描，请重试\"}");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    esp_err_t result=reply(req,!strncmp(json,"{\"error\":",9)?"503 Service Unavailable":"200 OK",json);
    free(json);return result;
}

/* Serial-only diagnostic: exercises the actual dual-stack HTTP socket and scan handler. */
void clock_http_scan_test(void)
{
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0){ESP_LOGE(TAG,"HTTPTEST socket failed");return;}
    struct timeval timeout={.tv_sec=6};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(80),.sin_addr.s_addr=htonl(0xc0a80401)};
    if(connect(fd,(struct sockaddr *)&addr,sizeof(addr))!=0){close(fd);ESP_LOGE(TAG,"HTTPTEST connect failed");return;}
    const char *request="GET /api/scan HTTP/1.0\r\nHost: 192.168.4.1\r\nConnection: close\r\n\r\n";
    if(send(fd,request,strlen(request),0)<0){close(fd);ESP_LOGE(TAG,"HTTPTEST send failed");return;}
    char *response=calloc(1,8192);size_t used=0;
    if(!response){close(fd);return;}
    for(;;){int n=recv(fd,response+used,8191-used,0);if(n<=0)break;used+=n;if(used>=8191)break;}
    close(fd);
    bool ok=strstr(response,"200 OK")&&strstr(response,"\"networks\":");
    ESP_LOGI(TAG,"HTTPTEST result=%s bytes=%u",ok?"PASS":"FAIL",(unsigned)used);
    if(!ok)ESP_LOGW(TAG,"HTTPTEST response: %.300s",response);
    free(response);
}

static cJSON *body_json(httpd_req_t *req)
{
    char content_type[40];
    if(httpd_req_get_hdr_value_str(req,"Content-Type",content_type,sizeof(content_type))!=ESP_OK ||
       strcmp(content_type,"application/json")!=0 || req->content_len<2 || req->content_len>256) return NULL;
    char body[257]; int used=0;
    while(used<req->content_len) {
        int got=httpd_req_recv(req,body+used,req->content_len-used);
        if(got<=0) return NULL;
        used+=got;
    }
    body[used]=0;
    return cJSON_ParseWithLength(body,used);
}

static esp_err_t set_time(httpd_req_t *req)
{
    if(!from_setup_ap(req)) return reply(req,"403 Forbidden","{\"error\":\"请连接设备热点后设置\"}");
    cJSON *body=body_json(req);
    cJSON *epoch=cJSON_GetObjectItemCaseSensitive(body,"epoch");
    bool ok=cJSON_IsNumber(epoch) && epoch->valuedouble>=1735689600.0 &&
            epoch->valuedouble<=4102444799.0 && clock_set_epoch((long long)epoch->valuedouble);
    cJSON_Delete(body);
    return reply(req,ok?"200 OK":"400 Bad Request",ok?"{\"ok\":true}":"{\"error\":\"时间无效\"}");
}

static void restart_later(void *arg)
{
    (void)arg; vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart();
}

static esp_err_t save_config(httpd_req_t *req)
{
    if(!from_setup_ap(req)) return reply(req,"403 Forbidden","{\"error\":\"请连接设备热点后设置\"}");
    cJSON *body=body_json(req);
    cJSON *ssid=cJSON_GetObjectItemCaseSensitive(body,"ssid");
    cJSON *password=cJSON_GetObjectItemCaseSensitive(body,"password");
    if(!cJSON_IsString(ssid)||!cJSON_IsString(password)||strlen(ssid->valuestring)<1||
       strlen(ssid->valuestring)>32||strlen(password->valuestring)>63||
       (strlen(password->valuestring)>0&&strlen(password->valuestring)<8)) {
        cJSON_Delete(body);
        return reply(req,"400 Bad Request","{\"error\":\"Wi-Fi 名称或密码长度无效\"}");
    }
    nvs_handle_t nvs;
    esp_err_t err=nvs_open("roundclock",NVS_READWRITE,&nvs);
    if(err==ESP_OK) {
        err=nvs_set_str(nvs,"ssid",ssid->valuestring);
        if(err==ESP_OK) err=nvs_set_str(nvs,"password",password->valuestring);
        if(err==ESP_OK) err=nvs_commit(nvs);
        nvs_close(nvs);
    }
    cJSON_Delete(body);
    if(err!=ESP_OK) return reply(req,"500 Internal Server Error","{\"error\":\"保存失败\"}");
    if(xTaskCreate(restart_later,"restart",2048,NULL,3,NULL)!=pdPASS)
        return reply(req,"500 Internal Server Error","{\"error\":\"请手动重启设备\"}");
    return reply(req,"200 OK","{\"ok\":true}");
}

void clock_network_start(void)
{
    scan_lock=xSemaphoreCreateMutex();
    if(!scan_lock)abort();
    setenv("TZ","CST-8",1); tzset();
    ESP_ERROR_CHECK(nvs_flash_init());
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open("roundclock",NVS_READWRITE,&nvs));
    size_t length;
    wifi_config_t station={0}; char ssid[33]={0},password[64]={0};
    length=sizeof(ssid); have_station=nvs_get_str(nvs,"ssid",ssid,&length)==ESP_OK && ssid[0];
    length=sizeof(password); nvs_get_str(nvs,"password",password,&length); nvs_close(nvs);
    memcpy(station.sta.ssid,ssid,strlen(ssid));
    memcpy(station.sta.password,password,strlen(password));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta(); esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL));
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    uint8_t mac[6]; ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_WIFI_SOFTAP));
    snprintf(clock_ap_ssid,sizeof(clock_ap_ssid),"RoundClock-%02X%02X",mac[4],mac[5]);
    wifi_config_t ap={0};
    memcpy(ap.ap.ssid,clock_ap_ssid,strlen(clock_ap_ssid));
    ap.ap.ssid_len=strlen(clock_ap_ssid); ap.ap.channel=1;
    ap.ap.max_connection=2; ap.ap.authmode=WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap));
    if(have_station) ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&station));
    esp_sntp_config_t ntp=ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    ntp.start=false; ntp.sync_cb=ntp_sync;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&ntp));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG,"Setup Wi-Fi: %s OPEN (no password) URL: http://192.168.4.1",clock_ap_ssid);
    httpd_handle_t server; httpd_config_t config=HTTPD_DEFAULT_CONFIG(); config.stack_size=6144;
    ESP_ERROR_CHECK(httpd_start(&server,&config));
    httpd_uri_t root={.uri="/",.method=HTTP_GET,.handler=root_get};
    httpd_uri_t time_uri={.uri="/api/time",.method=HTTP_POST,.handler=set_time};
    httpd_uri_t wifi_uri={.uri="/api/config",.method=HTTP_POST,.handler=save_config};
    httpd_uri_t scan_uri={.uri="/api/scan",.method=HTTP_GET,.handler=scan_get};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&time_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&wifi_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&scan_uri));
}
