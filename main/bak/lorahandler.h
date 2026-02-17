#ifndef __LORAHANDLER__
#define __LORAHANDLER__

#include <RadioLib.h>
#include "hal/ESP32S3Hal/ESP32S3Hal.hpp"   // include your HAL implementation


#ifdef __cplusplus
extern "C" {
#endif


void lorahandler_init(float freq, float bandwidth, int spreadingfactor, int codingrate, int power);
void lorahandler_start(void);

#ifdef __cplusplus
}
#endif

#endif