
#include "lorahandler.hpp"
#include "esp_log.h"
#include "homeapp.h"
#include "driver/gpio.h"
#include "clients.h"

static const char *TAG = "LORAHANDLER";

static Esp32S3Hal hal(RADIO_SCK, RADIO_MISO, RADIO_MOSI);
static Module mod(&hal, RADIO_NSS, RADIO_IRQ, RADIO_RST, RADIO_GPIO);
static SX1262 radio(&mod);
static char rxBuf[64]; // muista alustaa eka merkki nollaksi constructorissa
static bool pairingAllowed = true;


static char *interpret_lorapacket(struct fromlora *packet)
{
    static char buff[120];
    int len;

    len = sprintf(buff,"\"batt\":%d,\"dev\":\"%02x%02x%02x%02x\"", packet->battvoltage,
                                                                    packet->clientid[0],
                                                                    packet->clientid[1],
                                                                    packet->clientid[2],
                                                                    packet->clientid[3]);
    switch (packet->msgid)
    {
        case temperatures:
            ESP_LOGI(TAG, "Got temperatures, cnt=%d", packet->data.temperatures.cnt);
            sprintf(&buff[len],",\"temperatures\":[");
            len = strlen(buff);
            for (int i=0; i < packet->data.temperatures.cnt; i++)
            {
                if (i==4) break;
                sprintf(&buff[len],"%.2f,", packet->data.temperatures.measurements[i]);
                len = strlen(buff);
            }
            buff[len-1] = ']';
        break;

        case brightness:
            sprintf(&buff[len],",\"brightness\":[");
            len = strlen(buff);
            for (int i=0; i < packet->data.brightnesses.cnt; i++)
            {
                if (i==4) break;
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
                if (i==4) break;
                sprintf(&buff[len],"{\"value\":%.2f},", packet->data.humidities.measurements[i]);
                len = strlen(buff);
            }
            buff[len-1] = ']';
        break;
    }
    buff[len] = 0;
    return buff;
}

static void reader(void *arg)
{
    int16_t rxState;
    int timeoutCnt=0;
    struct tolora msg;
    struct client *client;

    ESP_LOGI(TAG, "Now in lorareader");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(80));
        rxState = radio.receive((uint8_t*)rxBuf, sizeof(rxBuf), 5000);
        if (rxState == RADIOLIB_ERR_NONE)
        {
            client = clients_find(((struct fromlora *) rxBuf)->clientid);
            if (client == NULL)
            {
                client = clients_new(((struct fromlora *) rxBuf)->clientid);
            }

            ESP_LOGI(TAG, "client pairstate = %d, pairingAllowed=%d", client->paired, pairingAllowed);
            if (!pairingAllowed && !client->paired)
            {
                ESP_LOGI(TAG, "skipping this client %s", client->friendlyname);
                continue;
            }

            timeoutCnt = 0;
            struct measurement meas;
            meas.id = LORA;
            meas.data.rssi = radio.getRSSI();
            meas.data.snr  = radio.getSNR();
            client->rssi = meas.data.rssi;
            client->snr  = meas.data.snr;
            time(&client->lastActive);
            client->powerUsed = ((struct fromlora *)rxBuf)->powerused;
            meas.data.count = client->powerUsed;
            ESP_LOGI(TAG, "client user power %u", client->powerUsed);

            if (meas.data.rssi < -100 && client->powerUsed < 16)
            {
                vTaskDelay(pdMS_TO_TICKS(300));
                msg.msgid = increasepower;
                radio.setOutputPower(client->powerUsed + 1);
                radio.transmit((unsigned char *) &msg, sizeof(struct tolora));
                ESP_LOGI(TAG, "rssi was %.1f, asking remote to increase power",meas.data.rssi);
                vTaskDelay(pdMS_TO_TICKS(300));
            }

            else if (meas.data.rssi > -90 && client->powerUsed > 1)
            {
                vTaskDelay(pdMS_TO_TICKS(300));
                msg.msgid = decreasepower;
                radio.setOutputPower(client->powerUsed + 1);
                radio.transmit((unsigned char *) &msg, sizeof(struct tolora));
                ESP_LOGI(TAG, "rssi was %.1f, asking remote to decrease power",meas.data.rssi);
                vTaskDelay(pdMS_TO_TICKS(300));
            }

            strcpy((char *)meas.data.recdata, interpret_lorapacket((struct fromlora *) rxBuf));
            xQueueSend(evt_queue, &meas,0);

            client->queuedCnt = uxQueueMessagesWaiting(client->send_queue);
            for (int i=0;i < client->queuedCnt; i++)
            {
                ESP_LOGI(TAG, "%u messages in sendqueue", client->queuedCnt);
                if (xQueueReceive(client->send_queue, &msg, 10))
                {
                    vTaskDelay(pdMS_TO_TICKS(300));
                    int16_t txState = radio.transmit((unsigned char *) &msg, sizeof(struct tolora));

                    if (txState == RADIOLIB_ERR_NONE)
                    {
                        switch (msg.msgid)
                        {
                            case pair:
                                ESP_LOGI(TAG, "pairing bridge to %s", client->friendlyname);
                                clients_pairclient(client);
                            break;

                            case changeinterval:
                                ESP_LOGI(TAG, "changing client %s interval to %d", client->friendlyname, msg.data.interval);
                                clients_setinterval(client, msg.data.interval);
                            break;

                            default:
                            break;
                        }
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
     LoraHandler(433.75, 25.0,  10, 6, 10);
}

LoraHandler::LoraHandler(float freq, float bandwidth, int spreadingfactor, int codingrate, int power)
{
    rxBuf[0]=0;
    int16_t state = radio.begin();

    if (state == RADIOLIB_ERR_NONE) {
        ESP_LOGI(TAG, "Radio initialized OK");
    } else {
        ESP_LOGE(TAG, "Radio init failed: %d", state);
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    radio.setFrequency(freq);
    radio.setBandwidth(bandwidth);
    radio.setSpreadingFactor(spreadingfactor);
    radio.setCodingRate(codingrate);
    radio.setOutputPower(power);
    radio.setCRC(true);
    
    vTaskDelay(pdMS_TO_TICKS(500));
}    

void LoraHandler::start()
{
    ESP_LOGI(TAG, "Starting lorareader");
    xTaskCreatePinnedToCore(reader,"lorareader", 4096 , NULL, 10, NULL, 1);
}

void LoraHandler::send(struct tolora *msg)
{
    struct client *client = clients_find(msg->devid);
    if (client != NULL)
    {
        xQueueSend(client->send_queue, msg,0);
    }
    else
    {
        ESP_LOGI(TAG,"client not found");
    }
}
