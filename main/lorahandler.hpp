#ifndef __LORAHANDLER__
#define __LORAHANDLER__

#include <RadioLib.h>
#include "hal/ESP32S3Hal/ESP32S3Hal.hpp"   // include your HAL implementation
#include "freertos/queue.h"

#define RADIO_NSS   (8)   // LoRa CS
#define RADIO_IRQ   (14)  // DIO1
#define RADIO_RST   (12)  // Reset
#define RADIO_GPIO  (13)  // Busy

#define RADIO_SCK   (9)
#define RADIO_MISO  (11)
#define RADIO_MOSI  (10)

class LoraHandler {
public:
    LoraHandler(void);
    LoraHandler(float freq, float bandwidth, int spreadingfactor, int codingrate, int power);
    void send(struct tolora *msg);
    void start();
};


#endif