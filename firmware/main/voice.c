#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_websocket_client.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "network.h"
#include "voice.h"
#include "voice_config.h"

#define AUDIO_RING_BYTES (96*1024)
static const char *TAG="voice";
static QueueHandle_t commands;
static SemaphoreHandle_t view_lock;
static RingbufHandle_t audio_ring;
static StaticRingbuffer_t audio_ring_storage;
static i2s_chan_handle_t mic,speaker;
static esp_websocket_client_handle_t websocket;
static volatile bool connected=false,session=false,recording=false,playing=false,audio_finished=false,abort_audio=false;
static volatile bool reconnect_hello=false,server_ready=false;
static bool continuous=false;
static volatile uint32_t revision=1;
static voice_state_t state=VOICE_OFF;
static char status_text[96]="",transcript[1024]="",reply_text[8192]="";
static int level=0,page=0;
static int scroll_offset=0;
static int64_t scroll_at=0;
enum {CMD_ENTER=1,CMD_LEAVE,CMD_TOGGLE,CMD_MICCHECK,CMD_BEEP,CMD_COMPLETE,CMD_COMPLETE_BEEP};

static void lock(void){xSemaphoreTake(view_lock,portMAX_DELAY);}
static void unlock(void){xSemaphoreGive(view_lock);}
static void status(voice_state_t next,const char *message)
{
    lock();state=next;snprintf(status_text,sizeof(status_text),"%s",message);++revision;unlock();
    ESP_LOGI(TAG,"State=%d %s",next,message);
}
static int utf8_size(const char *s)
{
    unsigned char c=(unsigned char)*s;
    int n=c<0x80?1:c<0xe0?2:c<0xf0?3:4;
    for(int i=1;i<n;++i)if(!s[i]||((unsigned char)s[i]&0xc0)!=0x80)return 1;
    return n;
}
static void copy_utf8(char *target,const char *source,size_t size)
{
    size_t used=0;
    while(*source){int n=utf8_size(source);if(used+n>=size)break;memcpy(target+used,source,n);used+=n;source+=n;}
    target[used]=0;
}
static int count_chars(const char *text){int n=0;while(*text){text+=utf8_size(text);++n;}return n;}
void voice_get_view(voice_view_t *view)
{
    lock();view->state=state;view->level=level;view->page=page;
    view->pages=(count_chars(reply_text)+39)/40;if(!view->pages)view->pages=1;
    if(page>=view->pages)page=view->page=view->pages-1;
    copy_utf8(view->status,status_text,sizeof(view->status));copy_utf8(view->transcript,transcript,sizeof(view->transcript));
    const char *start=reply_text;int skip=scroll_offset;
    while(*start&&skip-- >0)start+=utf8_size(start);
    const char *end=start;int remaining=40;while(*end&&remaining-- >0)end+=utf8_size(end);
    size_t bytes=end-start;if(bytes>=sizeof(view->reply))bytes=sizeof(view->reply)-1;
    memcpy(view->reply,start,bytes);view->reply[bytes]=0;unlock();
}
uint32_t voice_revision(void){return revision;}
bool voice_session_active(void){return session&&(recording||playing||state==VOICE_THINKING);}
void voice_next_page(void){lock();scroll_offset=0;page=0;scroll_at=esp_timer_get_time()+3000000;++revision;unlock();}
void voice_tick(void)
{
    int64_t now=esp_timer_get_time();
    lock();int total=count_chars(reply_text);
    if(session&&total>40&&now>=scroll_at&&scroll_offset<total-40){
        scroll_offset+=10;if(scroll_offset>total-40)scroll_offset=total-40;
        page=scroll_offset/40;scroll_at=now+1600000;++revision;
    }
    unlock();
}
static void queue(int command){xQueueSend(commands,&command,0);}
void voice_enter(void){queue(CMD_ENTER);}
void voice_leave(void){queue(CMD_LEAVE);}
void voice_toggle(void){queue(CMD_TOGGLE);}
void voice_notify_completion(void){queue(CMD_COMPLETE);}
void voice_notify_completion_kind(int kind){if(kind!=2)queue(kind==1?CMD_COMPLETE_BEEP:CMD_COMPLETE);}

static bool send_control(const char *json)
{
    if(!websocket||!connected)return false;
    return esp_websocket_client_send_text(websocket,json,strlen(json),pdMS_TO_TICKS(300))>=0;
}
static void discard_audio(void)
{
    size_t bytes;void *data;
    while((data=xRingbufferReceive(audio_ring,&bytes,0))!=NULL)vRingbufferReturnItem(audio_ring,data);
}
static void handle_json(const char *text)
{
    cJSON *body=cJSON_Parse(text);if(!body)return;
    cJSON *kind=cJSON_GetObjectItemCaseSensitive(body,"type"),*value=cJSON_GetObjectItemCaseSensitive(body,"text");
    if(!cJSON_IsString(kind)){cJSON_Delete(body);return;}
    const char *type=kind->valuestring;
    if(!strcmp(type,"ready")){server_ready=true;status(VOICE_IDLE,"轻触开始对话");}
    else if(!strcmp(type,"listening"))status(VOICE_LISTENING,"正在听，请说话");
    else if(!strcmp(type,"recording_end")){recording=false;status(VOICE_THINKING,"正在确认识别结果");}
    else if(!strcmp(type,"thinking"))status(VOICE_THINKING,"Hermes 正在思考");
    else if(!strcmp(type,"synthesizing")&&!playing)status(VOICE_THINKING,"正在生成语音");
    else if(!strcmp(type,"transcript")&&cJSON_IsString(value)){
        lock();copy_utf8(transcript,value->valuestring,sizeof(transcript));++revision;unlock();
    }else if(!strcmp(type,"text_delta")&&cJSON_IsString(value)){
        lock();size_t used=strlen(reply_text);copy_utf8(reply_text+used,value->valuestring,sizeof(reply_text)-used);++revision;unlock();
    }else if(!strcmp(type,"reply")&&cJSON_IsString(value)){
        lock();copy_utf8(reply_text,value->valuestring,sizeof(reply_text));++revision;unlock();
    }else if(!strcmp(type,"audio_start")){
        recording=false;audio_finished=false;abort_audio=false;playing=true;status(VOICE_SPEAKING,"正在回答");
    }else if(!strcmp(type,"audio_end")){audio_finished=true;if(!playing){continuous=false;status(VOICE_IDLE,"轻触继续，底部返回");}}
    else if(!strcmp(type,"no_speech")){recording=false;continuous=false;status(VOICE_IDLE,"未听清，轻触重试");}
    else if(!strcmp(type,"error")){
        recording=false;continuous=false;abort_audio=true;
        cJSON *message=cJSON_GetObjectItemCaseSensitive(body,"message");status(VOICE_ERROR,cJSON_IsString(message)?message->valuestring:"连接出现问题，请重试");
    }
    cJSON_Delete(body);
}
static void websocket_event(void *arg,esp_event_base_t base,int32_t id,void *event_data)
{
    (void)arg;(void)base;
    static char message[4096];static int used=0;static bool binary=false;
    if(id==WEBSOCKET_EVENT_CONNECTED){connected=true;reconnect_hello=true;ESP_LOGI(TAG,"NAS WebSocket connected");}
    else if(id==WEBSOCKET_EVENT_DISCONNECTED){connected=false;server_ready=false;recording=false;abort_audio=true;if(session)status(VOICE_ERROR,"语音连接已断开，正在重连");}
    else if(id==WEBSOCKET_EVENT_DATA){
        esp_websocket_event_data_t *event=event_data;
        if(event->op_code==2)binary=true;
        else if(event->op_code==1)binary=false;
        else if(event->op_code!=0)return;
        if(binary){
            if(playing&&!abort_audio&&event->data_len>0&&xRingbufferSend(audio_ring,event->data_ptr,event->data_len,pdMS_TO_TICKS(1000))!=pdTRUE){
                abort_audio=true;continuous=false;status(VOICE_ERROR,"音频缓冲超时，请重试");
            }
        }else{
            if(event->payload_offset==0)used=0;
            if(used+event->data_len<(int)sizeof(message)){
                memcpy(message+used,event->data_ptr,event->data_len);used+=event->data_len;message[used]=0;
                if(event->payload_offset+event->data_len>=event->payload_len)handle_json(message);
            }else used=0;
        }
    }
}

static void connect_server(void)
{
    if(websocket)return;
    if(!clock_wifi_connected){status(VOICE_ERROR,"请先连接家中 Wi-Fi");return;}
    status(VOICE_CONNECTING,"正在连接 NAS");
    esp_websocket_client_config_t config={.uri=VOICE_SERVER_URI,.headers=VOICE_AUTH_HEADER,
        .buffer_size=4096,.task_stack=8192,.network_timeout_ms=3000,.reconnect_timeout_ms=2000,.ping_interval_sec=10};
    websocket=esp_websocket_client_init(&config);
    if(!websocket){status(VOICE_ERROR,"无法创建语音连接");return;}
    ESP_ERROR_CHECK(esp_websocket_register_events(websocket,WEBSOCKET_EVENT_ANY,websocket_event,NULL));
    if(esp_websocket_client_start(websocket)!=ESP_OK){esp_websocket_client_destroy(websocket);websocket=NULL;status(VOICE_ERROR,"无法连接 NAS");}
}
static void start_recording(void)
{
    if(!session||!connected||!server_ready){connect_server();return;}
    gpio_set_level(GPIO_NUM_48,0);discard_audio();playing=false;audio_finished=false;abort_audio=false;
    lock();reply_text[0]=transcript[0]=0;page=0;scroll_offset=0;scroll_at=esp_timer_get_time()+3000000;++revision;unlock();
    if(send_control("{\"type\":\"start\"}")){recording=true;status(VOICE_LISTENING,"正在听，请说话");}
}
static void stop_session(void)
{
    session=false;recording=false;continuous=false;abort_audio=true;gpio_set_level(GPIO_NUM_48,0);
    if(websocket){send_control("{\"type\":\"cancel\"}");esp_websocket_client_stop(websocket);esp_websocket_client_destroy(websocket);websocket=NULL;}
    connected=server_ready=false;discard_audio();playing=false;status(VOICE_OFF,"");
}
static void beep(void)
{
    int16_t stereo[480*2];gpio_set_level(GPIO_NUM_48,1);
    for(int offset=0;offset<12000;offset+=480){
        for(int i=0;i<480;++i){int16_t sample=2500*sinf((offset+i)*6.2831853f*880/24000);stereo[2*i]=stereo[2*i+1]=sample;}
        size_t sent;i2s_channel_write(speaker,stereo,sizeof(stereo),&sent,1000);
    }
    vTaskDelay(pdMS_TO_TICKS(80));gpio_set_level(GPIO_NUM_48,0);ESP_LOGI(TAG,"Speaker beep complete");
}

static void completion_notification(void)
{
    char authorization[160];const char *start=strchr(VOICE_AUTH_HEADER,':');start=start?start+1:"";while(*start==' ')++start;
    size_t length=strcspn(start,"\r\n");if(length>=sizeof(authorization))length=sizeof(authorization)-1;
    memcpy(authorization,start,length);authorization[length]=0;
    esp_http_client_config_t config={.url=VOICE_HTTP_BASE_URL "/codex/completion.pcm",.timeout_ms=6000};
    esp_http_client_handle_t client=esp_http_client_init(&config);int16_t *mono=malloc(480*6);bool sounded=false;
    if(client&&mono){
        esp_http_client_set_header(client,"Authorization",authorization);
        if(esp_http_client_open(client,0)==ESP_OK){
            esp_http_client_fetch_headers(client);
            if(esp_http_client_get_status_code(client)==200){
                int pending=0,count;int16_t *stereo=mono+480;
                while((count=esp_http_client_read(client,(char *)mono+pending,960-pending))>0){
                    count+=pending;int samples=count/2;
                    for(int i=0;i<samples;++i)stereo[2*i]=stereo[2*i+1]=(int32_t)mono[i]*55/100;
                    gpio_set_level(GPIO_NUM_48,1);size_t sent;
                    if(i2s_channel_write(speaker,stereo,samples*4,&sent,1000)!=ESP_OK)break;
                    sounded=true;pending=count%2;if(pending)((char *)mono)[0]=((char *)mono)[count-1];
                }
            }
            esp_http_client_close(client);
        }
    }
    if(client)esp_http_client_cleanup(client);
    free(mono);
    vTaskDelay(pdMS_TO_TICKS(80));gpio_set_level(GPIO_NUM_48,0);
    if(!sounded)beep();
    ESP_LOGI(TAG,"Codex completion notification: %s",sounded?"speech":"beep fallback");
}

static void audio_worker(void *arg)
{
    (void)arg;int16_t input[320],stereo[960];bool was_recording=false;
    int warmup=0,speech_frames=0,silence_frames=0,record_frames=0;int64_t check_until=0,last_level=0;
    double check_peak=0,check_floor=1e9,raw_mean=0;float dc=0;int pending_completion=0;
    for(;;){
        int command;
        while(xQueueReceive(commands,&command,0)==pdTRUE){
            if(command==CMD_ENTER){session=true;continuous=false;connect_server();}
            else if(command==CMD_LEAVE)stop_session();
            else if(command==CMD_TOGGLE){
                if(recording){recording=false;send_control("{\"type\":\"end\"}");status(VOICE_THINKING,"Hermes 正在思考");}
                else if(playing||state==VOICE_THINKING){recording=false;abort_audio=true;playing=false;send_control("{\"type\":\"cancel\"}");continuous=false;status(VOICE_IDLE,"已停止，轻触继续");}
                else {continuous=true;start_recording();}
            }else if(command==CMD_MICCHECK){check_until=esp_timer_get_time()+3000000;check_peak=0;check_floor=1e9;ESP_LOGI(TAG,"MICCHECK started (3 seconds)");}
            else if(command==CMD_BEEP&&!recording&&!playing)beep();
            else if(command==CMD_COMPLETE)pending_completion=1;
            else if(command==CMD_COMPLETE_BEEP)pending_completion=2;
        }
        if(pending_completion&&!session&&!recording&&!playing){int kind=pending_completion;pending_completion=0;if(kind==2)beep();else completion_notification();}
        if(reconnect_hello&&connected){
            reconnect_hello=false;uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_STA);char hello[128];
            snprintf(hello,sizeof(hello),"{\"type\":\"hello\",\"device_id\":\"s3-%02x%02x%02x%02x%02x%02x\"}",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);send_control(hello);
        }
        if(session&&!websocket&&clock_wifi_connected)connect_server();
        if(abort_audio){gpio_set_level(GPIO_NUM_48,0);discard_audio();playing=false;abort_audio=false;}
        if(playing){
            size_t bytes=0;int16_t *mono=xRingbufferReceiveUpTo(audio_ring,&bytes,pdMS_TO_TICKS(20),960);
            if(mono){
                int count=bytes/2;for(int i=0;i<count;++i)stereo[2*i]=stereo[2*i+1]=(int32_t)mono[i]*55/100;
                vRingbufferReturnItem(audio_ring,mono);gpio_set_level(GPIO_NUM_48,1);
                size_t sent;esp_err_t err=i2s_channel_write(speaker,stereo,count*4,&sent,200);
                if(err!=ESP_OK){abort_audio=true;continuous=false;status(VOICE_ERROR,"扬声器输出失败");}
            }else if(audio_finished){
                vTaskDelay(pdMS_TO_TICKS(80));gpio_set_level(GPIO_NUM_48,0);playing=false;audio_finished=false;
                status(VOICE_IDLE,"轻触继续，底部返回");
                if(continuous&&session){vTaskDelay(pdMS_TO_TICKS(120));start_recording();}
            }
        }
        if(recording||check_until){
            if(recording&&!was_recording){warmup=3;speech_frames=silence_frames=record_frames=0;}
            size_t bytes=0;
            if(i2s_channel_read(mic,input,sizeof(input),&bytes,80)==ESP_OK&&bytes){
                int count=bytes/2;double power=0,sum=0;
                for(int i=0;i<count;++i){
                    sum+=input[i];dc+=.002f*(input[i]-dc);
                    float filtered=input[i]-dc;
                    if(filtered>32767)filtered=32767;
                    if(filtered< -32768)filtered=-32768;
                    input[i]=(int16_t)filtered;power+=(double)input[i]*input[i];
                }
                raw_mean=sum/count;
                double rms=sqrt(power/count);
                if(check_until&&rms>check_peak)check_peak=rms;
                if(check_until&&rms<check_floor)check_floor=rms;
                if(esp_timer_get_time()-last_level>100000){lock();level=(int)fmin(rms/80,100);++revision;unlock();last_level=esp_timer_get_time();}
                if(recording&&warmup>0)--warmup;
                else if(recording){
                    if(esp_websocket_client_send_bin(websocket,(char *)input,bytes,pdMS_TO_TICKS(250))<0){recording=false;continuous=false;status(VOICE_ERROR,"录音发送失败");}
                    ++record_frames;
                    if(rms>220){++speech_frames;silence_frames=0;}else ++silence_frames;
                    if((speech_frames>=5&&silence_frames>=25)||record_frames>=1000){recording=false;send_control("{\"type\":\"end\"}");status(VOICE_THINKING,"Hermes 正在思考");}
                }
            }
            if(check_until&&esp_timer_get_time()>check_until){ESP_LOGI(TAG,"MICCHECK peak_rms=%.1f floor_rms=%.1f raw_mean=%.1f",check_peak,check_floor,raw_mean);check_until=0;}
        }
        was_recording=recording;
        if(!recording&&!playing&&!check_until)vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void voice_diagnostic(const char *command)
{
    if(!strcmp(command,"MICCHECK"))queue(CMD_MICCHECK);
    else if(!strcmp(command,"AUDIOBEEP"))queue(CMD_BEEP);
    else if(!strcmp(command,"VOICEOPEN"))voice_enter();
    else if(!strcmp(command,"VOICETAP"))voice_toggle();
    else if(!strcmp(command,"VOICECLOSE"))voice_leave();
    else if(!strcmp(command,"CODEXNOTIFY"))voice_notify_completion();
    else if(!strcmp(command,"VOICESTATUS"))ESP_LOGI(TAG,"STATE=%d CONNECTED=%d READY=%d RECORDING=%d PLAYING=%d",state,connected,server_ready,recording,playing);
}
void voice_init(void)
{
    view_lock=xSemaphoreCreateMutex();commands=xQueueCreate(8,sizeof(int));
    uint8_t *storage=heap_caps_malloc(AUDIO_RING_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!storage||!view_lock||!commands)abort();
    audio_ring=xRingbufferCreateStatic(AUDIO_RING_BYTES,RINGBUF_TYPE_BYTEBUF,storage,&audio_ring_storage);
    if(!audio_ring)abort();
    i2s_chan_config_t rx=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);
    rx.dma_desc_num=4;rx.dma_frame_num=320;
    ESP_ERROR_CHECK(i2s_new_channel(&rx,NULL,&mic));
    i2s_pdm_rx_config_t pdm={.clk_cfg=I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg=I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_MONO),
        .gpio_cfg={.clk=45,.din=46,.invert_flags.clk_inv=false}};
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(mic,&pdm));ESP_ERROR_CHECK(i2s_channel_enable(mic));
    i2s_chan_config_t tx=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1,I2S_ROLE_MASTER);
    tx.dma_desc_num=4;tx.dma_frame_num=240;
    ESP_ERROR_CHECK(i2s_new_channel(&tx,&speaker,NULL));
    i2s_std_config_t standard={.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),
        .gpio_cfg={.mclk=I2S_GPIO_UNUSED,.bclk=18,.ws=16,.dout=17,.din=I2S_GPIO_UNUSED}};
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(speaker,&standard));ESP_ERROR_CHECK(i2s_channel_enable(speaker));
    if(xTaskCreate(audio_worker,"voice_audio",8192,NULL,5,NULL)!=pdPASS)abort();
    ESP_LOGI(TAG,"PDM microphone and I2S speaker initialized; audio ring in PSRAM");
}
