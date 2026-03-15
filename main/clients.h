#ifndef __CLIENT__
#define __CLIENT__

#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

#define LEN_ID 4

struct client 
{
    unsigned char clientid[LEN_ID];
    char friendlyname[20];
    QueueHandle_t send_queue;
    nvs_handle flash;
    int queuedCnt;
    bool paired;
    time_t lastActive;
    long interval;
    int powerUsed;
    float rssi;
    float snr;
};

#ifdef __cplusplus
extern "C" {
#endif

extern struct client *clients_find(unsigned char *dev);
extern struct client *clients_new(unsigned char *dev);
extern void clients_pairclient(struct client *c);
extern void clients_setinterval(struct client *c, unsigned int interval);
extern void clients_setfriendlyname(struct client *c, char *name);
extern void clients_init(nvs_handle nvsh, char *mqtt_prefix, char *appname, uint8_t *chipid);
extern void clients_publish_devices(esp_mqtt_client_handle_t client, uint8_t *chipid);
extern bool clients_isTableChanged(void);

#ifdef __cplusplus
}
#endif


#endif