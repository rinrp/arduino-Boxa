#ifndef NBIOT_H
#define NBIOT_H
#include <Arduino.h>
// ============================================================
#define LO_API_KEY      "4d248462f24c4f8d9a865c42019671bc"
#define LO_DEVICE_ID    "urn:lo:nsid:mqtt:RFID"
#define LO_STREAM_ID    "urn:lo:nsid:mqtt:RFID"
#define LO_MQTT_CLIENT  "ArduinoRFID"

#define LO_TOPIC_PUB    "dev/data"
#define LO_TOPIC_SUB    "dev/cmd"
#define LO_TOPIC_CMD_RES "dev/cmd/res"

#define LO_MQTT_HOST    "mqtt.liveobjects.orange-business.com"
#define LO_MQTT_PORT    8883
#define LO_MQTT_USER    "json+device"
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
//  Inițializare
// ============================================================

/**
 * Pornește inițializarea NB-IoT — NU blochează.
 * Apelați o singură dată din setup(), după Serial.begin().
 * Returnează imediat (după 500ms power-up modem).
 * Sistemul RFID și senzorii funcționează din primul moment.
 */
bool nbiot_init();

/**
 * Continuă inițializarea NB-IoT în background.
 * OBLIGATORIU: apelați în fiecare iterație de loop()
 * până când nbiot_isConnected() returnează true.
 * Non-blocker — durează microsecunde per apel.
 */
void nbiot_initTick();

// ============================================================
//  Publicare evenimente
// ============================================================

/**
 * Trimite un eveniment JSON pe topicul dev/data.
 *
 * Format JSON publicat:
 * {
 *   "s": "<streamId>",    // tipul evenimentului (EVT_*)
 *   "v": {
 *     "uid":   "<uid>",   // optional — UID card (poate fi "")
 *     "state": "<state>"  // starea curentă a sistemului
 *   }
 * }
 *
 * @param eventType  unul din EVT_* definiți mai sus
 * @param uid        UID-ul cardului implicat (sau "" dacă nu e cazul)
 * @param stateStr   numele stării curente (ex: "IDLE", "ALARM")
 */
void nbiot_publish(const char* eventType,
                   const char* uid,
                   const char* stateStr);

// ============================================================
//  Primire comenzi din cloud
// ===========================================================
void nbiot_checkCommands();

// ============================================================
//  Callback pentru comenzi primite
// ============================================================

/**
 * Tipuri de comenzi posibile returnate de callback.
 */
enum NbiotCmd {
    CMD_NONE,
    CMD_OPEN,
    CMD_CARD_ADD,
    CMD_CARD_REMOVE,
    CMD_STATUS
};

/**
 * Structura cu datele comenzii primite.
 */
struct NbiotCommand {
    NbiotCmd type;
    char     uid[15];  // "XX:XX:XX:XX\0" — valid doar pt CMD_CARD_*
};

/**
 * Tip pointer la funcție callback pentru comenzi.
 * main.cpp va implementa această funcție și o va înregistra.
 */
typedef void (*NbiotCommandCallback)(const NbiotCommand& cmd);

/** Înregistrează callback-ul pentru comenzi primite din cloud. */
void nbiot_setCommandCallback(NbiotCommandCallback cb);

// ============================================================
//  Utilitar: heartbeat periodic
void nbiot_heartbeatTick(const char* stateStr);

#define NBIOT_HEARTBEAT_MS  30000UL  // heartbeat la 30 secunde

// ============================================================
//  Status conexiune
// ============================================================

/** Returnează true dacă modulul este conectat la MQTT. */
bool nbiot_isConnected();

#endif // NBIOT_H