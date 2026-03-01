

#include <oled_display.h>
#include "driver/i2c_master.h"

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>

#include <stdlib.h>

#include "cJSON.h"
#include "lorahandler.hpp"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "esp_wifi_types.h"
#include "freertos/event_groups.h"


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "homeapp.h"
#include "flashmem.h"
#include "ota/ota.h"
#include "device/device.h"
#include "statistics/statistics.h"
#include "mqtt_client.h"
#include "apwebserver/server.h"
#include "factoryreset.h"


#define MSG_WAITING_GPIO   ((gpio_num_t) 6)
#define STATISTICS_INTERVAL 7200
#define ESP_INTR_FLAG_DEFAULT 0



#if CONFIG_EXAMPLE_WIFI_SCAN_METHOD_FAST
#define EXAMPLE_WIFI_SCAN_METHOD WIFI_FAST_SCAN
#elif CONFIG_EXAMPLE_WIFI_SCAN_METHOD_ALL_CHANNEL
#define EXAMPLE_WIFI_SCAN_METHOD WIFI_ALL_CHANNEL_SCAN
#endif

#if CONFIG_EXAMPLE_WIFI_CONNECT_AP_BY_SIGNAL
#define EXAMPLE_WIFI_CONNECT_AP_SORT_METHOD WIFI_CONNECT_AP_BY_SIGNAL
#elif CONFIG_EXAMPLE_WIFI_CONNECT_AP_BY_SECURITY
#define EXAMPLE_WIFI_CONNECT_AP_SORT_METHOD WIFI_CONNECT_AP_BY_SECURITY
#endif

#define WIFI_RECONNECT_RETRYCNT 50

#define HEALTHYFLAGS_WIFI 1
#define HEALTHYFLAGS_MQTT 2
#define HEALTHYFLAGS_NTP  4


struct netinfo {
    char *ssid;
    char *password;
    char *mqtt_server;
    char *mqtt_port;
    char *mqtt_prefix;
};


// globals

struct netinfo *comminfo;
QueueHandle_t evt_queue = NULL;
char jsondata[512];

static const char *TAG = "LORABRIDGE";
static bool isConnected = false;
static uint8_t healthyflags = 0;

static char statisticsTopic[64];
static char readTopic[64];
static char otaUpdateTopic[64];
static int retry_num = 0;
static char *program_version;
static char appname[20];
static LoraHandler lorahandler;

nvs_handle setup_flash;

static void sendInfo(esp_mqtt_client_handle_t client, uint8_t *chipid);

static void display_printf(int line, const char *format, ...)
{
    va_list args;
    char buffer[21];

    va_start(args, format);
    vsprintf(buffer, format, args);
    va_end(args);
    oled_draw_text(0, line, buffer);
}

static void display_text(const char *str)
{
    static int line = 0;

    vTaskDelay(pdMS_TO_TICKS(100));
    if (str == NULL)
    {
        oled_clear();
        vTaskDelay(pdMS_TO_TICKS(140));
        line = 0;
        return;
    }
    if (!line)
    {
        oled_clear();
        vTaskDelay(pdMS_TO_TICKS(140));
    }
    oled_draw_text(0, line++, str);
    if (line == 8) line = 0;
}

static void display_connections(void)
{
    display_printf(0, "%s %s %s", (healthyflags & HEALTHYFLAGS_WIFI) ? "WIFI " : "     ",
                                  (healthyflags & HEALTHYFLAGS_NTP)  ? "NTP  " : "     ",
                                  (healthyflags & HEALTHYFLAGS_MQTT) ? "MQTT" : "    ");
}

static void display_received(struct measurement *meas)
{
    display_printf(1,"rssi:%.1f snr:%.1f", meas->data.rssi, meas->data.snr);
    display_printf(2,"client power %d", meas->data.count);
}

static void display_sent(enum wifimsgid toloraid, unsigned char *targ, int numericval)
{
    switch (toloraid)
    {
        case changeinterval:
            display_printf(3,"chg interval %d", numericval);
        break;

        case displaytext:
            display_printf(3,"disp txt, len=%d", numericval);
        break;

        default:
            ESP_LOGI(TAG,"toloraid %d not handled in display_sent().");
    }
    display_printf(4,"targ=%0x%0x%0x%0x", targ[0],targ[1],targ[2],targ[3]);
}

static char *getJsonStr(cJSON *js, const char *name)
{
    cJSON *item = cJSON_GetObjectItem(js, name);

    if (item != NULL)
    {
        if (cJSON_IsString(item))
        {
            return item->valuestring;
        }
        else ESP_LOGI(TAG, "%s is not a string", name);
    }
    else ESP_LOGI(TAG,"%s not found from json", name);
    return (char *) "\0";
}

static bool getJsonInt(cJSON *js, const char *name, int *val)
{
    bool ret = false;

    cJSON *item = cJSON_GetObjectItem(js, name);
    if (item != NULL)
    {
        if (cJSON_IsNumber(item))
        {
            if (item->valueint != *val)
            {
                ret = true;
                *val = item->valueint;
            }
            else ESP_LOGI(TAG,"%s is not changed", name);
        }
        else ESP_LOGI(TAG,"%s is not a number", name);
    }
    else ESP_LOGI(TAG,"%s not found from json", name);
    return ret;
}



static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

// dp = '{"origin":"6ab8","dev":"aaaa","data":"----> testi <-----"}';


static bool handleJson(esp_mqtt_event_handle_t event, uint8_t *chipid)
{
    cJSON *root = cJSON_Parse(event->data);
    bool ret = false;
    char id[20];

    if (root != NULL)
    {
        strcpy(id,getJsonStr(root,"id"));
    }
    if (!strcmp(id,"otaupdate"))
    {
        char *fname = getJsonStr(root,"file");
        if (strlen(fname) > 5)
        {
            ota_start(fname);
            display_printf(7,"Starting ota update");
        }
    }
    else if (!strcmp(id,"sendtext"))
    {
        struct tolora msg;

        memcpy(msg.bridgeid, &chipid[4], 4);
        cJSON *p = cJSON_GetObjectItem(root, "msg");
        if (p != NULL)
        {
            char *devstr = getJsonStr(p,"dev");
            int devnum = (int) strtol(devstr,NULL,16);
            memcpy(&msg.devid, &devnum,4);
            msg.msgid = displaytext;
            strcpy(msg.data.text, getJsonStr(p,"data"));
            display_connections();
            display_sent(msg.msgid, msg.devid, strlen(msg.data.text));
            lorahandler.send(&msg);
            gpio_set_level(MSG_WAITING_GPIO, true);
        }
        else
        {
            ESP_LOGI(TAG,"field 'msg' not found from json");
            display_printf(7,"msg not found");
        }

    }
    else if (!strcmp(id,"changeinterval"))
    {
        struct tolora msg;

        memcpy(msg.bridgeid, &chipid[4], 4);
        cJSON *p = cJSON_GetObjectItem(root, "msg");
        if (p != NULL)
        {
            char *devstr = getJsonStr(p,"dev");
            int devnum = (int) strtol(devstr,NULL,16);
            memcpy(&msg.devid, &devnum,4);

            msg.msgid = changeinterval;
            msg.data.interval = 120; // default
            getJsonInt(p, "data", (int *) &msg.data.interval);
            display_connections();
            display_sent(msg.msgid, msg.devid, msg.data.interval);
            lorahandler.send(&msg);
            gpio_set_level(MSG_WAITING_GPIO, true);
        }
        else
        {
            ESP_LOGI(TAG,"field 'msg' not found from json");
            display_printf(7,"msg not found");
        }
    }
    cJSON_Delete(root);
    return ret;
}


/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        ESP_LOGI(TAG,"subscribing topic %s", readTopic);
        msg_id = esp_mqtt_client_subscribe(client, readTopic, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        display_printf(7,"           ");
        gpio_set_level(MSG_WAITING_GPIO, false);

        isConnected = true;
        device_sendstatus(client, comminfo->mqtt_prefix, appname, (uint8_t *) handler_args);
        sendInfo(client, (uint8_t *) handler_args);
        statistics_getptr()->connectcnt++;
        healthyflags |= HEALTHYFLAGS_MQTT;
        display_connections();
        break;

    case MQTT_EVENT_DISCONNECTED:
        healthyflags &= ~HEALTHYFLAGS_MQTT;
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        statistics_getptr()->disconnectcnt++;
        isConnected = false;
        display_connections();
        break;

    case MQTT_EVENT_SUBSCRIBED:
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        handleJson(event,(uint8_t *) handler_args );
        display_printf(7,"Received from json");
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
            display_printf(7,"Mqtt error");

        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}


void sntp_callback(struct timeval *tv)
{
    (void) tv;
    static bool firstSyncDone = false;

    if (!firstSyncDone)
    {
        firstSyncDone = true;
        healthyflags |= HEALTHYFLAGS_NTP;
        display_connections();
    }
}

static void sntp_start()
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    sntp_set_time_sync_notification_cb(sntp_callback);
}


int getWifiStrength(void)
{
    wifi_ap_record_t ap;

    if (!esp_wifi_sta_get_ap_info(&ap))
        return ap.rssi;
    return 0;
}


static void sendInfo(esp_mqtt_client_handle_t client, uint8_t *chipid)
{
    char infoTopic[42];

    sprintf(infoTopic,"%s/%s/%02x%02x%02x/info",
         comminfo->mqtt_prefix, appname, chipid[3],chipid[4],chipid[5]);
    sprintf(jsondata, "{\"dev\":\"%02x%02x%02x\",\"id\":\"info\",\"memfree\":%d,\"idfversion\":\"%s\",\"progversion\":\"%s\"}",
                chipid[3],chipid[4],chipid[5],
                esp_get_free_heap_size(),
                esp_get_idf_version(),
                program_version);
    esp_mqtt_client_publish(client, infoTopic, jsondata , 0, 0, 1);
    statistics_getptr()->sendcnt++;
}

static void sendtowifi(struct measurement *meas, esp_mqtt_client_handle_t client, uint8_t *chipid)
{
    char loraTopic[42];
    time_t now;

    time(&now);

    sprintf(loraTopic,"%s/%s/%02x%02x%02x/loradata",
         comminfo->mqtt_prefix, appname, chipid[3],chipid[4],chipid[5]);
    sprintf(jsondata, "{\"bridge\":\"%02x%02x%02x\",\"ts\":%jd,\"id\":\"loradata\",\"rssi\":%.1f,\"snr\":%.1f,\"data\":%s}",
                chipid[3],chipid[4],chipid[5],now,
                meas->data.rssi,
                meas->data.snr,
                meas->data.recdata);
    esp_mqtt_client_publish(client, loraTopic, jsondata , 0, 0, 1);
    statistics_getptr()->sendcnt++;
}


static esp_mqtt_client_handle_t mqtt_app_start(uint8_t *chipid)
{
    char client_id[128];
    char uri[64];
    char deviceTopic[42];
    
    sprintf(client_id,"client_id=%s%02x%02x%02x",
        comminfo->mqtt_prefix ,chipid[3],chipid[4],chipid[5]);
    sprintf(uri,"mqtt://%s:%s",comminfo->mqtt_server, comminfo->mqtt_port);

    ESP_LOGI(TAG,"built client id=[%s]",client_id);

    esp_mqtt_client_config_t mqtt_cfg;
    memset(&mqtt_cfg, 0, sizeof(esp_mqtt_client_config_t));

    mqtt_cfg.broker.address.uri = uri;
    mqtt_cfg.credentials.client_id = client_id;
    mqtt_cfg.session.last_will.topic = device_topic(comminfo->mqtt_prefix, deviceTopic, chipid);
    mqtt_cfg.session.last_will.msg = device_data(jsondata, chipid, appname, 0);
    mqtt_cfg.session.last_will.msg_len = strlen(jsondata);
    mqtt_cfg.session.last_will.qos = 0;
    mqtt_cfg.session.last_will.retain = 1;

    ESP_LOGI(TAG,"jsondata=[%s]",jsondata);

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, (esp_mqtt_event_id_t) ESP_EVENT_ANY_ID, mqtt_event_handler, chipid);
    esp_mqtt_client_start(client);
    return client;
}

static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id,void *event_data)
{
    switch (event_id)
    {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG,"WIFI CONNECTING");
        break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG,"WiFi CONNECTED");
            display_connections();
        break;

        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG,"WiFi lost connection");
            if(retry_num < WIFI_RECONNECT_RETRYCNT)
            {
                esp_wifi_connect();
                retry_num++;
                ESP_LOGI(TAG,"Retrying to Connect");
            }
            display_connections();
        break;

        default:
            ESP_LOGI(TAG,"unknown wifi event %d %x", event_id, event_id);
            ESP_LOGI(TAG,"description %s", esp_err_to_name(event_id));
    }
}

static void ip_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id,void *event_data)
{
    switch (event_id)
    {
        case IP_EVENT_STA_GOT_IP:
            ESP_LOGI(TAG,"Wifi got IP\n");
            retry_num = 0;
            healthyflags |= HEALTHYFLAGS_WIFI;
            display_connections();
        break;

        case WIFI_EVENT_STA_DISCONNECTED:
            healthyflags &= ~HEALTHYFLAGS_WIFI;
            healthyflags &= ~HEALTHYFLAGS_NTP;
            display_connections();
            break;

        default:
            ESP_LOGI(TAG,"unknown ip event %d %x", event_id, event_id);
            ESP_LOGI(TAG,"description %s", esp_err_to_name(event_id));
    }
}


void wifi_connect(char *ssid, char *password)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_initiation);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL);

    wifi_config_t wifi_configuration;
    memset(&wifi_configuration,0,sizeof(wifi_config_t));
    wifi_configuration.sta.threshold.rssi = -127;
    wifi_configuration.sta.threshold.authmode = WIFI_AUTH_OPEN;


    strcpy((char*)wifi_configuration.sta.ssid, ssid);
    strcpy((char*)wifi_configuration.sta.password, password);

    esp_wifi_set_config(WIFI_IF_STA, &wifi_configuration);
    esp_wifi_start();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_connect();
}



struct netinfo *get_networkinfo()
{
    static struct netinfo ni;
    const char *default_ssid = "XXXXXXXX";

    nvs_handle wifi_flash = flash_open("wifisetup");
    if (wifi_flash == -1) return NULL;

    ni.ssid = flash_read_str(wifi_flash, "ssid",default_ssid, 20);
    if (!strcmp(ni.ssid, default_ssid))
        return NULL;

    ni.password    = flash_read_str(wifi_flash, "password","pass", 20);
    ni.mqtt_server = flash_read_str(wifi_flash, "mqtt_server","test.mosquitto.org", 20);
    ni.mqtt_port   = flash_read_str(wifi_flash, "mqtt_port","1883", 6);
    ni.mqtt_prefix = flash_read_str(wifi_flash, "mqtt_prefix","home/esp", 20);
    return &ni;
}

static void get_appname(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    strncpy(appname,app_desc->project_name,20);
}


extern "C" void app_main(void)
{
    uint8_t chipid[8];
    time_t now, prevStatsTs;
    esp_efuse_mac_get_default(chipid);

    
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    ESP_LOGI(TAG, "chipid = %02x%02x%02x%02x", chipid[4],chipid[5],chipid[6],chipid[7]);

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    gpio_reset_pin(MSG_WAITING_GPIO);
    gpio_set_direction(MSG_WAITING_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(MSG_WAITING_GPIO, true);

    ESP_ERROR_CHECK(oled_init());

    get_appname();

    comminfo = get_networkinfo();
    if (comminfo == NULL)
    {
        display_text("Connect your wifi to:");
        display_text(CONFIG_ESP_WIFI_SSID);
        display_text("Browse to address:");
        display_text("192.168.4.1");
        server_init();
    }
    else
    {
        setup_flash = flash_open("storage");
        gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
        factoryreset_init();

        wifi_connect(comminfo->ssid, comminfo->password);
        esp_wifi_set_ps(WIFI_PS_NONE);
        evt_queue = xQueueCreate(10, sizeof(struct measurement));
        esp_mqtt_client_handle_t client = mqtt_app_start(chipid);
        sntp_start();

        lorahandler = LoraHandler();

        ESP_LOGI(TAG, "[APP] All init done, app_main, last line.");

        sprintf(statisticsTopic,"%s/%s/%02x%02x%02x/statistics",
            comminfo->mqtt_prefix, appname, chipid[3],chipid[4],chipid[5]);
        ESP_LOGI(TAG,"statisticsTopic=[%s]", statisticsTopic);

        sprintf(otaUpdateTopic,"%s/%s/%02x%02x%02x/otaupdate",
            comminfo->mqtt_prefix, appname, chipid[3],chipid[4],chipid[5]);

        sprintf(readTopic,"%s/%s/%02x%02x%02x/send",
            comminfo->mqtt_prefix, appname, chipid[3],chipid[4],chipid[5]);

        prevStatsTs = 0;
        program_version = ota_init(comminfo->mqtt_prefix, appname, chipid);
        if (!statistics_init(comminfo->mqtt_prefix, appname, chipid))
        {
            ESP_LOGE(TAG,"failed in statistics init");
        }
        lorahandler.start();

        while (1)
        {
            struct measurement meas;

            time(&now);
            if ((now - statistics_getptr()->started > 20) &&
                (healthyflags == (HEALTHYFLAGS_WIFI | HEALTHYFLAGS_MQTT | HEALTHYFLAGS_NTP)))
            {
                ota_cancel_rollback();
            }

            if (now > MIN_EPOCH)
            {
                if (statistics_getptr()->started < MIN_EPOCH)
                {
                    statistics_getptr()->started = now;
                }
                if (now - prevStatsTs >= STATISTICS_INTERVAL)
                {
                    if (isConnected)
                    {
                        statistics_send(client);
                        prevStatsTs = now;
                    }
                }
            }

            if (xQueueReceive(evt_queue, &meas, STATISTICS_INTERVAL * 1000 / portTICK_PERIOD_MS)) {
                time(&now);
                uint16_t qcnt = uxQueueMessagesWaiting(evt_queue);
                if (qcnt > statistics_getptr()->maxQElements)
                {
                    statistics_getptr()->maxQElements = qcnt;
                }

                switch (meas.id) {
                    case OTA:
                        ota_status_publish(&meas, client);
                    break;

                    case LORA:
                        display_connections();
                        display_received(&meas);
                        if (isConnected) sendtowifi(&meas, client, chipid);
                    break;

                    case CLEARDISP:
                        display_text(NULL);
                    break;

                    default:
                        ESP_LOGD(TAG, "unknown data type" );
                }
            }
            else
            {   // timeout
                ESP_LOGI(TAG,"timeout");
            }
        }
    }
}
