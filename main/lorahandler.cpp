
#include "lorahandler.hpp"
#include "esp_log.h"
#include "homeapp.h"
#include "driver/gpio.h"


static const char *TAG = "LORAHANDLER";

static Esp32S3Hal hal(RADIO_SCK, RADIO_MISO, RADIO_MOSI);
static Module mod(&hal, RADIO_NSS, RADIO_IRQ, RADIO_RST, RADIO_GPIO);
static SX1262 radio(&mod);
static char rxBuf[64]; // muista alustaa eka merkki nollaksi constructorissa
static QueueHandle_t send_queue;    


static char *interpret_lorapacket(struct fromlora *packet)
{
    static char buff[120];
    int len;

    len = sprintf(buff,"{\"batt\":%d,\"dev\":\"%02x%02x%02x\"", packet->battvoltage,
                                                                packet->clientid[0],
                                                                packet->clientid[1],
                                                                packet->clientid[2]);
    switch (packet->msgid)
    {
        case temperatures:
            ESP_LOGI(TAG, "Got temperetures, cnt=%d", packet->data.temperatures.cnt);
            sprintf(&buff[len],",\"temperature\":[");
            len = strlen(buff);
            for (int i=0; i < packet->data.temperatures.cnt; i++)
            {
                if (i==8) break;
                sprintf(&buff[len],"{\"value\":%.2f},", packet->data.temperatures.measurements[i]);
                len = strlen(buff);
            }
            buff[len-1] = ']';
        break;

        case brightness:
            sprintf(&buff[len],",\"brightness\":[");
            len = strlen(buff);
            for (int i=0; i < packet->data.brightnesses.cnt; i++)
            {
                if (i==8) break;
                sprintf(&buff[len],"{\"value\":%d},", packet->data.brightnesses.measurements[i]);
                len = strlen(buff);
            }
            buff[len-1] = ']';
        break;

        case humidity:
            sprintf(&buff[len],",\"humidity\":[");
            len = strlen(buff);
            for (int i=0; i < packet->data.humidities.cnt; i++)
            {
                if (i==8) break;
                sprintf(&buff[len],"{\"value\":%.2f},", packet->data.humidities.measurements[i]);
                len = strlen(buff);
            }
            buff[len-1] = ']';
        break;
    }
    buff[len] = '}';
    buff[len+1] = 0;
    return buff;
}

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
            meas.data.rssi = radio.getRSSI();
            meas.data.snr  = radio.getSNR();
            strcpy((char *)meas.data.recdata, interpret_lorapacket((struct fromlora *) rxBuf));
            xQueueSend(evt_queue, &meas,0);
            struct tolora msg;
            uint16_t qcnt = uxQueueMessagesWaiting(send_queue);
            for (int i=0;i<qcnt;i++)
            {
                ESP_LOGI(TAG, "%u messages in sendqueue", qcnt);
                if (xQueueReceive(send_queue, &msg, 10))
                {
                    vTaskDelay(pdMS_TO_TICKS(300));
                    int16_t txState = radio.transmit((unsigned char *) &msg, sizeof(struct tolora));

                    if (txState == RADIOLIB_ERR_NONE)
                    {
                        ESP_LOGI(TAG, "datalen %d", sizeof(struct tolora));
                        ESP_LOGI(TAG, "** Message sent successfully **");
                    }
                }
                else
                {
                    ESP_LOGE(TAG, "Nothing to send");
                }
            }
            gpio_set_level(MSG_WAITING_GPIO, false);
        }    
        else if (rxState == RADIOLIB_ERR_RX_TIMEOUT)
        {
            ESP_LOGI(TAG, "RX Timeout");
            vTaskDelay(pdMS_TO_TICKS(100));   // after timeout there is a bigger timeout, this causes sync
            timeoutCnt++;
        }
        else
        {
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

    send_queue = xQueueCreate(10, sizeof(struct tolora));
    
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
    xTaskCreatePinnedToCore(reader,"lorareader", 4096 , NULL, 10, NULL, 1);
}

void LoraHandler::send(struct tolora *msg)
{
    xQueueSend(send_queue, msg,0);
}
