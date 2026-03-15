
#include <string.h>
#include "clients.h"
#include "flashmem.h"
#include "esp_log.h"
#include "homeapp.h"
#include "statistics/statistics.h"

static const char *TAG = "CLIENTS";

#define MAX_QUEUED 10
#define MAX_CLIENTS 5

static struct client *clients[MAX_CLIENTS];
static int clientCnt = 0;
static nvs_handle main_partition;
static char clientsTopic[80];
static bool clientTableChanged = false;

struct client *clients_find(unsigned char *dev)
{
    ESP_LOGI(TAG,"Searching dev %02x%02x%02x%02x from clients table..", dev[0],dev[1],dev[2],dev[3]);
    if (clientCnt)
    {
        for (int i=0; i < clientCnt; i++)
        {
            if (!memcmp(clients[i]->clientid, dev, LEN_ID))
            {
                ESP_LOGI(TAG,"Found!");
                return clients[i];
            }    
        }
    }
    ESP_LOGE(TAG,"Not found");
    return NULL;
}

void clients_pairclient(struct client *c)
{
    flash_write(c->flash, "devpaired", 1);
    flash_commitchanges(c->flash);
    c->paired = true;
    clientTableChanged = true;
}

struct client *clients_new(unsigned char *dev)
{
    struct client *p;
    uint32_t *uDev = (uint32_t *) dev;
    char name[13];

    p = (struct client *) malloc(sizeof(struct client));
    if (p != NULL)
    {
        sprintf(name,"dev%02x%02x%02x%02x",dev[0],dev[1],dev[2],dev[3]);
        ESP_LOGI(TAG,"---> writing variable [%s] to main partition. value is %u", name, *uDev);
        flash_write32(main_partition, name, *uDev); // write device as numeric value *uDev
        flash_commitchanges(main_partition);
        memcpy(p->clientid, dev, LEN_ID);
        sprintf(name,"cpa%02x%02x%02x%02x",dev[0],dev[1],dev[2],dev[3]);
        ESP_LOGI(TAG,"Client partition name is %s", name);
        p->flash = flash_open(name);
        p->send_queue = xQueueCreate(MAX_QUEUED, sizeof(struct tolora));
        p->paired = false;
        p->lastActive = 0;
        p->interval = 120;
        p->powerUsed = 2;
        sprintf(p->friendlyname,"%02x%02x%02x%02x",dev[0],dev[1],dev[2],dev[3]);
        clients[clientCnt] = p;
        clientCnt++;
        clientTableChanged = true;
    }
    return p;
}

void clients_setinterval(struct client *c, unsigned int interval)
{
    ESP_LOGI(TAG,"Saving interval %d to flash", interval);
    c->interval = interval;
    flash_write(c->flash, "interval", c->interval);
    flash_commitchanges(c->flash);
    clientTableChanged = true;
}


void clients_setfriendlyname(struct client *c, char *name)
{
    ESP_LOGI(TAG,"Saving friendlyname %s to flash", name);
    strcpy(c->friendlyname, name);
    flash_write_str(c->flash, "name", name);
    flash_commitchanges(c->flash);
    clientTableChanged = true;
}

bool clients_isTableChanged(void)
{
    bool ret = clientTableChanged;
    clientTableChanged = false;
    return ret;
}

void clients_publish_devices(esp_mqtt_client_handle_t client, uint8_t *chipid)
{
    time_t ts;
    int len;

    time(&ts);
    len=sprintf(jsondata, "{\"dev\":\"%02x%02x%02x\",\"id\":\"devices\",\"ts\":%jd,\"items\":[",
                                                chipid[3],chipid[4],chipid[5], ts);
    ESP_LOGI(TAG,"Publish devices, cnt = %d", clientCnt);
    for (int i=0; i < clientCnt; i++)
    {
        len += sprintf(&jsondata[len],"{\"addr\":\"%02x%02x%02x%02x\",\"name\":\"%s\",\"paired\":\"%d\",\"interval\":\"%ld\"},",
                                        clients[i]->clientid[0],
                                        clients[i]->clientid[1],
                                        clients[i]->clientid[2],
                                        clients[i]->clientid[3],
                                        clients[i]->friendlyname,
                                        clients[i]->paired,
                                        clients[i]->interval);
    }
    strcpy(&jsondata[len-1],"]}");
    esp_mqtt_client_publish(client, clientsTopic, jsondata , 0, 0, 1);
    statistics_getptr()->sendcnt++;
}

void clients_init(nvs_handle nvsh, char *mqtt_prefix, char *appname, uint8_t *chipid)
{
    nvs_iterator_t it = NULL;
    struct client *p;
    nvs_entry_info_t info;
    char cpa[13];

    sprintf(clientsTopic,"%s/%s/%02x%02x%02x/devices",mqtt_prefix, appname, chipid[3],chipid[4],chipid[5]);
    main_partition = nvsh;
    ESP_LOGI(TAG,"Searching clients from flash main partition");
    esp_err_t res = nvs_entry_find_in_handle(nvsh, NVS_TYPE_ANY, &it);
    for (clientCnt = 0; clientCnt < MAX_CLIENTS; clientCnt++)
    {
        if (res != ESP_OK) break;
        nvs_entry_info(it,&info);
        ESP_LOGI(TAG,"Found key [%s] from main partition", info.key);
        if (!memcmp(info.key,"dev",3))
        {
            ESP_LOGI(TAG,"Client partition name is [%s]", info.key);
            p = (struct client *) malloc(sizeof(struct client));
            if (p==NULL) break;
            uint32_t uDev = flash_read32(nvsh, info.key, 0);
            memcpy(p->clientid, &uDev, LEN_ID);
            ESP_LOGI(TAG,"--> Got dev [%s]::%u as device ", info.key, uDev);
            if (uDev != 0)
            {
                sprintf(cpa,"cpa%02x%02x%02x%02x",p->clientid[0], p->clientid[1], p->clientid[2], p->clientid[3]);
                ESP_LOGI(TAG,"This is client namespace %s", cpa);
                p->send_queue = xQueueCreate(MAX_QUEUED, sizeof(struct tolora));
                p->flash = flash_open(cpa);
                p->paired = flash_read(p->flash, "devpaired", 0);
                p->interval = flash_read(p->flash, "interval", 120);
                p->lastActive = 0;
                p->powerUsed = 0; // should this be stored in flash ?
                sprintf(p->friendlyname,"%02x%02x%02x%02x",p->clientid[0], p->clientid[1], p->clientid[2], p->clientid[3]);
                strcpy(p->friendlyname, flash_read_str(p->flash,"name", p->friendlyname, 20));
                clients[clientCnt] = p;
            }    
        }    
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
}

        