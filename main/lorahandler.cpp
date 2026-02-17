
#include "lorahandler.hpp"
#include "esp_log.h"
#include "homeapp.h"


static const char *TAG = "LORAHANDLER";

static Esp32S3Hal hal(RADIO_SCK, RADIO_MISO, RADIO_MOSI);
static Module mod(&hal, RADIO_NSS, RADIO_IRQ, RADIO_RST, RADIO_GPIO);
static SX1262 radio(&mod);
static char rxBuf[64]; // muista alustaa eka merkki nollaksi constructorissa
static QueueHandle_t send_queue;    

static void reader(void *arg)
{
    int16_t rxState;
    int timeoutCnt=0;

    ESP_LOGI(TAG, "Now in lorareader");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(80));
        rxState = radio.receive((uint8_t*)rxBuf, sizeof(rxBuf), 5000);
        if (rxState == RADIOLIB_ERR_NONE) 
        {
            timeoutCnt = 0;
            ESP_LOGI(TAG, "RX: %s", rxBuf);
            struct measurement meas;
            meas.id = LORA;
            strcpy(meas.data.loradata, rxBuf);
            xQueueSend(evt_queue, &meas,0);
            struct sendmsg msg;
            if (xQueueReceive(send_queue, &msg, 10)) 
            {
                vTaskDelay(pdMS_TO_TICKS(300));
                int16_t txState = radio.transmit(msg.data);
                vTaskDelay(pdMS_TO_TICKS(60));
                if (txState == RADIOLIB_ERR_NONE) 
                {
                    ESP_LOGI(TAG, "%s", msg.data);
                    ESP_LOGI(TAG, "** Message sent successfully **");
                }
            }
            else
            {
                ESP_LOGE(TAG, "Nothing to send");
            }    
        }    
        else if (rxState == RADIOLIB_ERR_RX_TIMEOUT) 
        {
            ESP_LOGI(TAG, "RX Timeout");
            vTaskDelay(pdMS_TO_TICKS(100));   // after timeout there is a bigger timeout, this causes sync
            timeoutCnt++;
        } else {
            ESP_LOGE(TAG, "RX Error:%d", rxState);
            vTaskDelay(pdMS_TO_TICKS(100));   // after timeout there is a bigger timeout, this causes sync
        }
        if (timeoutCnt > 10)
        {
            struct measurement meas;
            meas.id = CLEARDISP;
            xQueueSend(evt_queue, &meas,0);
        }        
    }
}

LoraHandler::LoraHandler(void)
{
    LoraHandler(433.5, 250.0, 10, 6, 10);
}

LoraHandler::LoraHandler(float freq, float bandwidth, int spreadingfactor, int codingrate, int power)
{
    rxBuf[0]=0;
    int16_t state = radio.begin();

    send_queue = xQueueCreate(3, sizeof(struct sendmsg));
    
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

void LoraHandler::start()
{
    ESP_LOGI(TAG, "Starting lorareader");
    xTaskCreate(reader,"lorareader", 4096 , NULL, 10, NULL);
}

void LoraHandler::send(struct sendmsg *msg)
{
    xQueueSend(send_queue, msg,0);
}
