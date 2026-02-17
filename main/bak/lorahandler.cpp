
#include "lorahandler.h"
#include "esp_log.h"
#include "homeapp.h"

#define RADIO_NSS   (8)   // LoRa CS
#define RADIO_IRQ   (14)  // DIO1
#define RADIO_RST   (12)  // Reset
#define RADIO_GPIO  (13)  // Busy

#define RADIO_SCK   (9)
#define RADIO_MISO  (11)
#define RADIO_MOSI  (10)


static const char *TAG = "LORAHANDLER";


char rxBuf[64] = {0};

static Esp32S3Hal hal(RADIO_SCK, RADIO_MISO, RADIO_MOSI);
static Module mod(&hal, RADIO_NSS, RADIO_IRQ, RADIO_RST, RADIO_GPIO);
static SX1262 radio(&mod);


static void lorareader(void *arg)
{
    int16_t rxState;

    ESP_LOGI(TAG, "Now in lorareader");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(80));
        rxState = radio.receive((uint8_t*)rxBuf, sizeof(rxBuf), 1500);
        if (rxState == RADIOLIB_ERR_NONE) 
        {
            ESP_LOGI(TAG, "RX: %s", rxBuf);
            struct measurement meas;
            meas.id = LORA;
            strcpy(meas.data.loradata, rxBuf);
            xQueueSend(evt_queue, &meas,0);
        }    
        else if (rxState == RADIOLIB_ERR_RX_TIMEOUT) 
        {
            ESP_LOGI(TAG, "RX Timeout");
            vTaskDelay(pdMS_TO_TICKS(1000));   // after timeout there is a bigger timeout, this causes sync
        } else {
            ESP_LOGE(TAG, "RX Error:%d", rxState);
            vTaskDelay(pdMS_TO_TICKS(1000));   // after timeout there is a bigger timeout, this causes sync
        }
    }
}


void lorahandler_init(float freq, float bandwidth, int spreadingfactor, int codingrate, int power)
{
      
    int16_t state = radio.begin();
    
    if (state == RADIOLIB_ERR_NONE) {
        ESP_LOGI(TAG, "Radio initialized OK");
    } else {
        ESP_LOGE(TAG, "Radio init failed: %d", state);
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    vTaskDelay(pdMS_TO_TICKS(1500));

    radio.setFrequency(freq);
    radio.setBandwidth(bandwidth);
    radio.setSpreadingFactor(spreadingfactor);
    radio.setCodingRate(codingrate);
    radio.setOutputPower(power);
    radio.setCRC(true);
    
    vTaskDelay(pdMS_TO_TICKS(1500));
}    

void lorahandler_start(void)
{
    ESP_LOGI(TAG, "Starting lorareader");
    xTaskCreate(lorareader,"lorareader", 4096 , NULL, 10, NULL);
}
