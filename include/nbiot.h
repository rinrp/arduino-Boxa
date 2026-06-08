#ifndef NBIOT_H
#define NBIOT_H

#include <Arduino.h>

// ============================================================
//  nbiot.h — Modul NB-IoT Quectel BC92 + Orange Live Objects
// ============================================================
//
// Hardware:
//   BC92 TX -> Arduino RX SoftwareSerial = D3
//   BC92 RX -> Arduino TX SoftwareSerial = D2
//
// În config.h:
//   MODEM_TX_PIN = 2
//   MODEM_RX_PIN = 3
//
// SoftwareSerial:
//   SoftwareSerial s_modem(MODEM_RX_PIN, MODEM_TX_PIN);
//
// ============================================================


// ============================================================
//  Orange Live Objects — credentiale RFID
// ============================================================

#define LO_API_KEY       "4d248462f24c4f8d9a865c42019671bc"

#define LO_DEVICE_ID     "RFID"
#define LO_STREAM_ID     "RFID"
#define LO_MQTT_CLIENT   "RFID"

#define LO_MQTT_HOST     "liveobjects.orange-business.com"
#define LO_MQTT_PORT     8883
#define LO_MQTT_USER     "json+device"

// Topic pentru publicare date către Live Objects
#define LO_TOPIC_PUB     "dev/v1/data/rawjson/version1"

// Topic pentru comenzi din Live Objects către dispozitiv
#define LO_TOPIC_SUB     "dev/cmd"

// Topic pentru confirmări comenzi
#define LO_TOPIC_CMD_RES "dev/cmd/res"

// SMS fallback către Live Objects
#define LO_SMS_ADMIN_NR  "3523"


// ============================================================
//  Tipuri de evenimente publicate
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
//  Comenzi primite din cloud
// ============================================================

enum NbiotCmd {
    CMD_NONE,
    CMD_OPEN,
    CMD_CARD_ADD,
    CMD_CARD_REMOVE,
    CMD_STATUS
};

struct NbiotCommand {
    NbiotCmd type;
    char uid[15];   // format "CA:FD:A1:80"
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

#define NBIOT_HEARTBEAT_MS 30000UL

#endif // NBIOT_H