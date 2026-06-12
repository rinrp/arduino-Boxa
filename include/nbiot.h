#ifndef NBIOT_H
#define NBIOT_H

#include <Arduino.h>

// ============================================================
//  nbiot.h — Quectel BC92 + Orange Live Objects
//  BUILD: MQTT_RX_PARSE_UID_AND_PUBLISH_OK
// ============================================================

#define LO_API_KEY       "4d248462f24c4f8d9a865c42019671bc"

#define LO_DEVICE_ID     "RFID"
#define LO_STREAM_ID     "RFID"
#define LO_MQTT_CLIENT   "RFID"

#define LO_MQTT_HOST     "liveobjects.orange-business.com"
#define LO_MQTT_PORT     8883
#define LO_MQTT_USER     "json+device"

#define LO_TOPIC_PUB     "dev/v1/data/rawjson/version1"
#define LO_TOPIC_SUB     "dev/cmd"
#define LO_TOPIC_CMD_RES "dev/cmd/res"

// 0 = SMS dezactivat, 1 = SMS activ pentru evenimente critice
#define ENABLE_SMS_FALLBACK 0
#define LO_SMS_ADMIN_NR  "3523"

// ============================================================
//  Evenimente
// ============================================================
#define EVT_ACCESS_GRANTED   "access_granted"
#define EVT_ACCESS_DENIED    "access_denied"
#define EVT_DOOR_OPEN        "door_open"
#define EVT_DOOR_CLOSED      "door_closed"
#define EVT_ALARM_START      "alarm_start"
#define EVT_ALARM_STOP       "alarm_stop"
#define EVT_SYSTEM_STATUS    "system_status"
#define EVT_CARD_ADDED       "card_added"
#define EVT_CARD_REMOVED     "card_removed"
#define EVT_REMOTE_OPEN      "remote_open"

// ============================================================
//  Comenzi cloud
// ============================================================
enum NbiotCmd : byte {
    CMD_NONE,
    CMD_OPEN,
    CMD_CARD_ADD,
    CMD_CARD_REMOVE,
    CMD_STATUS
};

struct NbiotCommand {
    NbiotCmd type;
    char uid[15];   // "CA:FD:A1:80\0"
    char cid[18];   // cid Live Objects pastrat ca text, ca sa evitam overflow int
};

typedef void (*NbiotCommandCallback)(const NbiotCommand& cmd);

// ============================================================
//  API public
// ============================================================
bool nbiot_init();
void nbiot_initTick();

void nbiot_publish(const char* eventType,
                   const char* uid,
                   const char* stateStr);

void nbiot_checkCommands();
void nbiot_setCommandCallback(NbiotCommandCallback cb);
void nbiot_heartbeatTick(const char* stateStr);
bool nbiot_isConnected();

#define NBIOT_HEARTBEAT_MS 120000UL

#endif // NBIOT_H
