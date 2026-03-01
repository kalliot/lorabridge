
#ifndef __HOMEAPP__
#define __HOMEAPP__

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "flashmem.h"



// message betweed two lora devices, this is in binary format.
enum loramsgid
{
    temperatures,   // remote device is sending a set of temperatures
    brightness,     // remote device sends brightness
    humidity,       // remote device sends humidity
    timeout         // bridge may send a timeout config parameter to remote device.
};

enum wifimsgid
{
    increasepower,
    decreasepower,
    changeinterval,
    pair,
    displaytext
};

#pragma pack(1)

struct temperature
{
    int cnt;
    float measurements[8];
};

struct humidity
{
    int cnt;
    float measurements[8];
};

struct brightness
{
    int cnt;
    int measurements[8];
};


// max payload size is 51 bytes. this means whole fromlora and tolora struct
struct fromlora
{
    int len;
    int battvoltage;
    unsigned char clientid[3];
    unsigned char bridgeid[3];
    unsigned char powerused;
    unsigned char msgid;
    union
    {
        struct temperature temperatures;
        struct humidity humidities;
        struct brightness brightnesses;
        unsigned char settarget[4];
        char displaytext[20];
    } data;
};


struct tolora
{
    unsigned char bridgeid[4];
    unsigned char devid[4];
    enum wifimsgid msgid;
        union {
        char text[51-9];
        unsigned int interval;
    } data;
};


enum meastype
{
    OTA,
    LORA,
    CLEARDISP
};

// measurement is sent out to wifi, everything needs to be converted to json first.
struct measurement {
    enum meastype id;
    int gpio;
    int err;
    struct
    {
        int count;
        float rssi;
        float snr;
        char recdata[120];
    } data;
};
#pragma pack()

extern QueueHandle_t evt_queue;
extern char jsondata[];
extern uint16_t sendcnt;
extern nvs_handle setup_flash;

#define MSG_WAITING_GPIO   ((gpio_num_t) 6)
#define MIN_EPOCH   1650000000

#endif