#ifndef NBIOT_H
#define NBIOT_H

#include <Arduino.h>

// ============================================================
//  nbiot.h — Modul NB-IoT Quectel BC92 + MQTT Live Objects
//
//  Hardware:
//    BC92 TX → Arduino RX (D3 = MODEM_RX_PIN)
//    BC92 RX → Arduino TX (D2 = MODEM_TX_PIN)
//    Comunicație via SoftwareSerial
//
//  Protocol: MQTT over NB-IoT
//  Platformă: Orange Live Objects
//
// ============================================================

// ============================================================
//  !! PLACEHOLDER-E — înlocuiește cu valorile tale reale !!
//
//  LO_API_KEY    → cheia API din Live Objects
//                  (Settings → API Keys → Create)
//                  Folosită ca parolă MQTT.
//
//  LO_DEVICE_ID  → ID-ul dispozitivului tău din Live Objects
//                  (Devices → My Devices → Device ID)
//                  Folosit ca Client ID MQTT.
//
//  LO_TOPIC_PUB  → topicul pe care publici date
//                  Standard Live Objects: "dev/data"
//
//  LO_TOPIC_SUB  → topicul pe care primești comenzi
//                  Standard Live Objects: "dev/cmd"
//
//  LO_MQTT_HOST  → brokerul MQTT Live Objects
//                  Standard: "liveobjects.orange-business.com"
//
//  LO_MQTT_PORT  → portul MQTT (1883 fără TLS)
// ============================================================

//  !! CREDENTIALE LIVE OBJECTS — nu le distribui public !!
//
//  LO_API_KEY     → cheia API (parola MQTT)
//  LO_DEVICE_ID   → ID-ul dispozitivului (folosit la identificare)
//  LO_MQTT_CLIENT → Client ID MQTT (unic per conexiune)
//  LO_STREAM_ID   → Stream ID pentru date publicate
//  LO_TOPIC_PUB   → topic publish date senzori
//  LO_TOPIC_SUB   → topic subscribe comenzi
//  LO_MQTT_HOST   → broker MQTT Live Objects
//  LO_MQTT_PORT   → port MQTT (1883 fara TLS)
//  LO_MQTT_USER   → username MQTT Live Objects (fix, nu modifica)

#define LO_API_KEY      "8d0264da509344169bb3fa89b071df72"
#define LO_DEVICE_ID    "urn:lo:nsid:mqtt:RFID"
#define LO_MQTT_CLIENT  "ArduinoRFID"
#define LO_STREAM_ID    "urn:lo:nsid:mqtt:RFID"
#define LO_TOPIC_PUB    "dev/data"
#define LO_TOPIC_SUB    "dev/cmd"
#define LO_MQTT_HOST    "liveobjects.orange-business.com"
#define LO_MQTT_PORT    1883

// Username MQTT Live Objects — nu modifica
#define LO_MQTT_USER    "json+device"

// ============================================================
//  Fallback SMS
//  Trimis pentru evenimente critice când MQTT e offline.
//
//  LO_SMS_ADMIN_NR → numărul tău de telefon (format internațional)
//                    ex: "+40712345678"
//                    Înlocuiește cu numărul tău real!
// ============================================================
#define LO_SMS_ADMIN_NR  "+40750288935"

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
// ============================================================

/**
 * Verifică dacă au sosit comenzi pe topicul dev/cmd.
 * Apelați în fiecare iterație de loop().
 * Non-blocker — citește doar ce e disponibil în buffer.
 *
 * Comenzi suportate (JSON):
 *   {"cmd":"open"}                        → deschide yala
 *   {"cmd":"card_add","uid":"XX:XX:XX:XX"} → adaugă card
 *   {"cmd":"card_remove","uid":"XX:XX:XX:XX"} → elimină card
 *   {"cmd":"status"}                      → publică starea curentă
 *
 * Rezultatul comenzii este returnat prin callback-uri
 * (vezi nbiot_setCommandCallback).
 */
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
// ============================================================

/**
 * Trimite periodic starea sistemului (heartbeat).
 * Apelați în loop() cu starea curentă.
 * Interval configurat în NBIOT_HEARTBEAT_MS.
 */
void nbiot_heartbeatTick(const char* stateStr);

#define NBIOT_HEARTBEAT_MS  30000UL  // heartbeat la 30 secunde

// ============================================================
//  Status conexiune
// ============================================================

/** Returnează true dacă modulul este conectat la MQTT. */
bool nbiot_isConnected();

#endif // NBIOT_H