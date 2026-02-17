
#ifndef __HOMEAPP__
#define __HOMEAPP__

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "flashmem.h"


enum meastype
{
    COUNT,
    TEMPERATURE,
    STATE,
    OTA,
    LORA,
    CLEARDISP
};

struct measurement {
    enum meastype id;
    int gpio;
    int err;
    union {
        int count;
        bool state;
        float temperature;
        char loradata[32];
    } data;
};

struct sendmsg {
    char targ[6];
    char data[64];
};


extern QueueHandle_t evt_queue;
extern char jsondata[];
extern uint16_t sendcnt;
extern uint16_t sensorerrors;
extern nvs_handle setup_flash;

#define COUNTER_GPIO       19
//#define COUNTER_ACTIVE_GPIO  7
#define MIN_EPOCH   1650000000

#endif