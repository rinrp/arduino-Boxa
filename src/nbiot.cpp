#include "nbiot.h"
#include "config.h"
#include <SoftwareSerial.h>

// ============================================================
//  nbiot.cpp — Quectel BC92 + Orange Live Objects MQTT/SMS
// ============================================================

#define MODEM_BAUD 9600

#define MQTT_RECONNECT_MAX_ATTEMPTS 5
#define MQTT_RECONNECT_COOLDOWN_MS  5000UL

// SoftwareSerial(rx, tx)
// BC92 TX -> Arduino RX = MODEM_RX_PIN
// BC92 RX -> Arduino TX = MODEM_TX_PIN
static SoftwareSerial s_modem(MODEM_RX_PIN, MODEM_TX_PIN);


// ============================================================
//  State machine inițializare modem / MQTT
// ============================================================

enum InitState {
    INIT_IDLE,

    INIT_AT_TEST,
    INIT_ECHO_OFF,
    INIT_APN,
    INIT_CEREG,
    INIT_CSQ,

    INIT_MQTT_VERSION,
    INIT_MQTT_SSL,
    INIT_MQTOPEN,
    INIT_MQTCONN,
    INIT_MQTSUB,

    // Deconectare completă MQTT înainte de reconnect
    INIT_MQTT_UNSUB,
    INIT_MQTT_DISC,
    INIT_MQTT_CLOSE,
    INIT_RECONNECT_COOLDOWN,

    INIT_DONE,
    INIT_FAILED
};

static InitState s_initState = INIT_IDLE;

static unsigned long s_initStepMs = 0;
static unsigned long s_initStepTimeout = 0;

static byte s_atRetries = 0;
static byte s_stepRetries = 0;
static byte s_ceregRetries = 0;
static byte s_mqttReconnectAttempts = 0;

static bool s_connected = false;
static bool s_network_ready = false;

static unsigned long s_lastHeartbeatMs = 0;

static NbiotCommandCallback s_cmdCallback = nullptr;


// ============================================================
//  Buffere
// ============================================================

static char s_atBuf[160];
static byte s_atPos = 0;

static char s_mqttBuf[180];
static byte s_mqttPos = 0;


// ============================================================
//  Helper buffer AT
// ============================================================

static void clearAtBuffer() {
    while (s_modem.available()) {
        s_modem.read();
    }

    s_atPos = 0;
    memset(s_atBuf, 0, sizeof(s_atBuf));
}


// ============================================================
//  Helper comenzi AT
// ============================================================

static void atWrite(const char* cmd) {
    clearAtBuffer();

    s_modem.println(cmd);

    Serial.print(F("[AT>>] "));
    Serial.println(cmd);

    delay(100);
}

static void atWrite(const __FlashStringHelper* cmd) {
    clearAtBuffer();

    s_modem.println(cmd);

    Serial.print(F("[AT>>] "));
    Serial.println(cmd);

    delay(100);
}

static bool atReadCheck(const char* expect) {
    delay(50);

    while (s_modem.available() && s_atPos < sizeof(s_atBuf) - 1) {
        char c = s_modem.read();

        if (c == '\r') continue;

        s_atBuf[s_atPos++] = c;
        s_atBuf[s_atPos] = '\0';
    }

    if (strstr(s_atBuf, expect)) {
        Serial.print(F("[AT<<] "));
        Serial.println(s_atBuf);
        return true;
    }

    return false;
}

static bool atHasError() {
    return strstr(s_atBuf, "ERROR") != nullptr ||
           strstr(s_atBuf, "+CME ERROR") != nullptr ||
           strstr(s_atBuf, "+CMS ERROR") != nullptr;
}

static void printRawIfAny() {
    if (s_atPos > 0) {
        Serial.print(F("[AT RAW] "));
        Serial.println(s_atBuf);
    }
}


// ============================================================
//  AT blocking helper pentru SMS / publish
// ============================================================

static bool atSendBlocking(const char* cmd,
                           const char* expect,
                           unsigned long timeoutMs)
{
    while (s_modem.available()) {
        s_modem.read();
    }

    if (cmd && cmd[0] != '\0') {
        s_modem.println(cmd);

        Serial.print(F("[AT>>] "));
        Serial.println(cmd);
    }

    char buf[160];
    byte pos = 0;
    memset(buf, 0, sizeof(buf));

    unsigned long start = millis();

    while (millis() - start < timeoutMs) {
        while (s_modem.available() && pos < sizeof(buf) - 1) {
            char c = s_modem.read();

            if (c == '\r') continue;

            buf[pos++] = c;
            buf[pos] = '\0';

            if (strstr(buf, expect)) {
                Serial.print(F("[AT<<] "));
                Serial.println(buf);
                return true;
            }

            if (strstr(buf, "ERROR") ||
                strstr(buf, "+CME ERROR") ||
                strstr(buf, "+CMS ERROR"))
            {
                Serial.print(F("[AT<<] "));
                Serial.println(buf);
                return false;
            }
        }
    }

    Serial.println(F("[AT] timeout blocker"));
    return false;
}

static bool waitPrompt(unsigned long timeoutMs) {
    unsigned long start = millis();

    while (millis() - start < timeoutMs) {
        while (s_modem.available()) {
            char c = s_modem.read();

            if (c == '>') {
                return true;
            }
        }
    }

    return false;
}


// ============================================================
//  Parsare CSQ
// ============================================================

static int parseCsqRssi() {
    const char* p = strstr(s_atBuf, "+CSQ:");

    if (!p) return -1;

    p += 5;

    while (*p == ' ') {
        p++;
    }

    return atoi(p);
}


// ============================================================
//  Helper MQTT
// ============================================================

static void sendQmtOpen() {
    clearAtBuffer();

    s_modem.print(F("AT+QMTOPEN=0,\""));
    s_modem.print(LO_MQTT_HOST);
    s_modem.print(F("\","));
    s_modem.print(LO_MQTT_PORT);
    s_modem.println();

    Serial.print(F("[AT>>] AT+QMTOPEN=0,\""));
    Serial.print(LO_MQTT_HOST);
    Serial.print(F("\","));
    Serial.println(LO_MQTT_PORT);

    delay(100);
}

static void sendQmtConn() {
    clearAtBuffer();

    s_modem.print(F("AT+QMTCONN=0,\""));
    s_modem.print(LO_MQTT_CLIENT);
    s_modem.print(F("\",\""));
    s_modem.print(LO_MQTT_USER);
    s_modem.print(F("\",\""));
    s_modem.print(LO_API_KEY);
    s_modem.println(F("\""));

    Serial.print(F("[AT>>] AT+QMTCONN=0,\""));
    Serial.print(LO_MQTT_CLIENT);
    Serial.print(F("\",\""));
    Serial.print(LO_MQTT_USER);
    Serial.println(F("\",\"***\""));

    delay(100);
}

static void sendQmtSub() {
    clearAtBuffer();

    s_modem.print(F("AT+QMTSUB=0,1,\""));
    s_modem.print(LO_TOPIC_SUB);
    s_modem.println(F("\",1"));

    Serial.print(F("[AT>>] AT+QMTSUB=0,1,\""));
    Serial.print(LO_TOPIC_SUB);
    Serial.println(F("\",1"));

    delay(100);
}

static void sendQmtUnsub() {
    clearAtBuffer();

    s_modem.print(F("AT+QMTUNS=0,2,\""));
    s_modem.print(LO_TOPIC_SUB);
    s_modem.println(F("\""));

    Serial.print(F("[AT>>] AT+QMTUNS=0,2,\""));
    Serial.print(LO_TOPIC_SUB);
    Serial.println(F("\""));

    delay(100);
}

static void sendQmtDisc() {
    clearAtBuffer();

    s_modem.println(F("AT+QMTDISC=0"));

    Serial.println(F("[AT>>] AT+QMTDISC=0"));

    delay(100);
}

static void sendQmtClose() {
    clearAtBuffer();

    s_modem.println(F("AT+QMTCLOSE=0"));

    Serial.println(F("[AT>>] AT+QMTCLOSE=0"));

    delay(100);
}


// ============================================================
//  Reconnect complet MQTT
// ============================================================

static void beginFullMqttReconnect(const __FlashStringHelper* reason) {
    Serial.print(F("[NB-IoT] Reconnect MQTT complet cerut: "));
    Serial.println(reason);

    s_connected = false;

    if (s_mqttReconnectAttempts >= MQTT_RECONNECT_MAX_ATTEMPTS) {
        Serial.println(F("[NB-IoT] EROARE: reconnect MQTT esuat dupa 5 incercari."));
        Serial.println(F("[NB-IoT] Sistemul ramane functional LOCAL."));
        s_initState = INIT_FAILED;
        return;
    }

    s_mqttReconnectAttempts++;

    Serial.print(F("[NB-IoT] Reconnect attempt "));
    Serial.print(s_mqttReconnectAttempts);
    Serial.print(F("/"));
    Serial.println(MQTT_RECONNECT_MAX_ATTEMPTS);

    s_stepRetries = 0;
    s_initStepMs = millis();
    s_initStepTimeout = 3000;

    s_initState = INIT_MQTT_UNSUB;
    sendQmtUnsub();
}


// ============================================================
//  SMS fallback către Live Objects
// ============================================================

static void smsSend(const char* eventType,
                    const char* uid,
                    const char* stateStr)
{
    Serial.print(F("[SMS] Trimit catre "));
    Serial.print(LO_SMS_ADMIN_NR);
    Serial.print(F(": "));
    Serial.print(eventType);
    Serial.print(F(" / "));
    Serial.println(stateStr);

    atSendBlocking("AT+CGSMS=2", "OK", 1500);
    atSendBlocking("AT+CMGF=1", "OK", 2000);
    atSendBlocking("AT+CSCS=\"GSM\"", "OK", 2000);

    char cmd[36];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", LO_SMS_ADMIN_NR);

    if (!atSendBlocking(cmd, ">", 5000)) {
        Serial.println(F("[SMS] EROARE: modemul nu a oferit prompt >"));
        s_modem.write(27);
        return;
    }

    s_modem.print(F("EVT:"));
    s_modem.print(eventType);

    s_modem.print(F(",ST:"));
    s_modem.print(stateStr);

    if (uid && uid[0] != '\0') {
        s_modem.print(F(",UID:"));
        s_modem.print(uid);
    }

    delay(300);
    s_modem.write(26);

    char buf[120];
    byte pos = 0;
    memset(buf, 0, sizeof(buf));

    unsigned long start = millis();

    while (millis() - start < 12000) {
        while (s_modem.available() && pos < sizeof(buf) - 1) {
            char c = s_modem.read();

            if (c == '\r') continue;

            buf[pos++] = c;
            buf[pos] = '\0';
        }

        if (strstr(buf, "+CMGS:") || strstr(buf, "OK")) {
            Serial.println(F("[SMS] Trimis cu succes!"));
            return;
        }

        if (strstr(buf, "ERROR") || strstr(buf, "+CMS ERROR")) {
            Serial.print(F("[SMS] EROARE: "));
            Serial.println(buf);
            return;
        }
    }

    Serial.println(F("[SMS] timeout trimitere"));
}


// ============================================================
//  Inițializare
// ============================================================

bool nbiot_init() {
    s_modem.begin(MODEM_BAUD);
    delay(800);

    Serial.println(F("[NB-IoT] Pornire initializare non-blocanta..."));
    Serial.println(F("[NB-IoT] RFID/senzori functioneaza imediat."));
    Serial.println(F("[NB-IoT] Conectare cloud in background..."));

    s_connected = false;
    s_network_ready = false;

    s_atRetries = 0;
    s_stepRetries = 0;
    s_ceregRetries = 0;
    s_mqttReconnectAttempts = 0;

    s_initState = INIT_AT_TEST;
    s_initStepMs = millis();
    s_initStepTimeout = 5000;

    atWrite("AT");

    return true;
}


// ============================================================
//  State machine inițializare
// ============================================================

void nbiot_initTick() {
    if (s_initState == INIT_DONE || s_initState == INIT_FAILED) return;
    if (s_initState == INIT_IDLE) return;

    unsigned long now = millis();

    switch (s_initState) {

        case INIT_AT_TEST:
            if (atReadCheck("OK")) {
                Serial.println(F("[NB-IoT] Modem raspunde!"));

                s_initState = INIT_ECHO_OFF;
                s_initStepMs = now;
                s_initStepTimeout = 3000;
                s_stepRetries = 0;

                atWrite("ATE0");
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_atRetries++;

                if (s_atRetries >= 5) {
                    Serial.println(F("[NB-IoT] EROARE: modem nu raspunde dupa 5 incercari"));
                    Serial.println(F("[NB-IoT] Verifica TX/RX, alimentare, antena, S201, S101."));
                    Serial.println(F("[NB-IoT] Sistem functional local. SMS/MQTT indisponibil."));
                    s_initState = INIT_FAILED;
                } else {
                    Serial.print(F("[NB-IoT] Reincercare AT "));
                    Serial.print(s_atRetries);
                    Serial.println(F("/5..."));

                    s_initStepMs = now;
                    atWrite("AT");
                }
            }
            break;


        case INIT_ECHO_OFF:
            if (atReadCheck("OK")) {
                Serial.println(F("[NB-IoT] Echo disabled."));

                s_initState = INIT_APN;
                s_initStepMs = now;
                s_initStepTimeout = 10000;
                s_stepRetries = 0;

                atWrite(F("AT+QCGDEFCONT=\"IP\",\"net\""));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;

                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB-IoT] EROARE: ATE0 esuat"));
                    printRawIfAny();
                    s_initState = INIT_FAILED;
                } else {
                    s_initStepMs = now;
                    atWrite(F("ATE0"));
                }
            }
            break;


        case INIT_APN:
            if (atReadCheck("OK")) {
                Serial.println(F("[NB-IoT] APN configurat. Verific retea..."));

                s_initState = INIT_CEREG;
                s_initStepMs = now;
                s_initStepTimeout = 5000;
                s_ceregRetries = 0;

                atWrite(F("AT+CEREG?"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;

                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB-IoT] EROARE: configurare APN esuata"));
                    printRawIfAny();
                    s_initState = INIT_FAILED;
                } else {
                    Serial.print(F("[NB-IoT] Reincercare APN "));
                    Serial.print(s_stepRetries);
                    Serial.println(F("/3..."));

                    s_initStepMs = now;
                    atWrite(F("AT+QCGDEFCONT=\"IP\",\"net\""));
                }
            }
            break;


        case INIT_CEREG:
            if (atReadCheck("+CEREG: 0,1") ||
                atReadCheck("+CEREG: 0,5") ||
                atReadCheck("+CEREG: 1,1") ||
                atReadCheck("+CEREG: 1,5"))
            {
                Serial.println(F("[NB-IoT] Inregistrat in retea!"));
                Serial.println(F("[NB-IoT] Verific nivel semnal cu AT+CSQ..."));

                s_network_ready = true;

                s_initState = INIT_CSQ;
                s_initStepMs = now;
                s_initStepTimeout = 5000;
                s_stepRetries = 0;

                atWrite(F("AT+CSQ"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_ceregRetries++;

                if (s_ceregRetries >= 12) {
                    Serial.println(F("[NB-IoT] EROARE: retea indisponibila / no coverage"));
                    printRawIfAny();
                    s_initState = INIT_FAILED;
                } else {
                    Serial.print(F("[NB-IoT] Reincercare CEREG "));
                    Serial.print(s_ceregRetries);
                    Serial.println(F("/12..."));

                    s_initStepMs = now;
                    atWrite(F("AT+CEREG?"));
                }
            }
            break;


        case INIT_CSQ:
            if (atReadCheck("OK")) {
                int rssi = parseCsqRssi();

                Serial.print(F("[NB-IoT] CSQ RSSI = "));
                Serial.println(rssi);

                if (rssi == 99 || rssi < 5) {
                    Serial.println(F("[NB-IoT] Semnal slab sau necunoscut. Reincerc CSQ..."));

                    s_stepRetries++;

                    if (s_stepRetries >= 5) {
                        Serial.println(F("[NB-IoT] EROARE: semnal insuficient pentru MQTT"));
                        s_initState = INIT_FAILED;
                    } else {
                        s_initStepMs = now;
                        atWrite(F("AT+CSQ"));
                    }
                } else {
                    Serial.println(F("[NB-IoT] Semnal OK. Pregatesc MQTT 3.1.1..."));

                    delay(300);

                    s_initState = INIT_MQTT_VERSION;
                    s_initStepMs = now;
                    s_initStepTimeout = 5000;
                    s_stepRetries = 0;

                    atWrite(F("AT+QMTCFG=\"version\",0,1"));
                }
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;

                if (s_stepRetries >= 5) {
                    Serial.println(F("[NB-IoT] EROARE: AT+CSQ fara raspuns"));
                    printRawIfAny();
                    s_initState = INIT_FAILED;
                } else {
                    s_initStepMs = now;
                    atWrite(F("AT+CSQ"));
                }
            }
            break;


        case INIT_MQTT_VERSION:
            if (atReadCheck("OK")) {
                Serial.println(F("[NB-IoT] MQTT version setat la 3.1.1"));

                s_initState = INIT_MQTT_SSL;
                s_initStepMs = now;
                s_initStepTimeout = 5000;
                s_stepRetries = 0;

                atWrite(F("AT+QMTCFG=\"ssl\",0,1,1,0"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;

                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB-IoT] EROARE: setare MQTT version esuata"));
                    printRawIfAny();
                    beginFullMqttReconnect(F("QMTCFG version failed"));
                } else {
                    s_initStepMs = now;
                    atWrite(F("AT+QMTCFG=\"version\",0,1"));
                }
            }
            break;


        case INIT_MQTT_SSL:
            if (atReadCheck("OK")) {
                Serial.println(F("[NB-IoT] SSL/TLS activat pentru MQTT 8883."));
                Serial.println(F("[NB-IoT] Deschid socket MQTT..."));

                s_initState = INIT_MQTOPEN;
                s_initStepMs = now;
                s_initStepTimeout = 30000;
                s_stepRetries = 0;

                sendQmtOpen();
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;

                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB-IoT] EROARE: activare SSL/TLS MQTT esuata"));
                    printRawIfAny();
                    beginFullMqttReconnect(F("QMTCFG ssl failed"));
                } else {
                    s_initStepMs = now;
                    atWrite(F("AT+QMTCFG=\"ssl\",0,1,1,0"));
                }
            }
            break;


        case INIT_MQTOPEN:
            if (atReadCheck("+QMTOPEN: 0,0")) {
                Serial.println(F("[NB-IoT] TCP/MQTTS conectat!"));
                Serial.println(F("[NB-IoT] Autentificare MQTT..."));

                s_initState = INIT_MQTCONN;
                s_initStepMs = now;
                s_initStepTimeout = 30000;
                s_stepRetries = 0;

                sendQmtConn();
            }

            // A primit +QMTOPEN dar cu alt cod decât 0
            else if (strstr(s_atBuf, "+QMTOPEN: 0,")) {
                Serial.println(F("[NB-IoT] EROARE QMTOPEN:"));
                Serial.println(s_atBuf);

                beginFullMqttReconnect(F("QMTOPEN result error"));
            }

            // ERROR / CME ERROR
            else if (atHasError()) {
                Serial.println(F("[NB-IoT] EROARE QMTOPEN:"));
                Serial.println(s_atBuf);

                beginFullMqttReconnect(F("QMTOPEN ERROR"));
            }

            // Timeout
            else if (now - s_initStepMs > s_initStepTimeout) {
                Serial.println(F("[NB-IoT] EROARE: QMTOPEN timeout."));
                printRawIfAny();

                beginFullMqttReconnect(F("QMTOPEN timeout"));
            }
            break;


        case INIT_MQTCONN:
            if (atReadCheck("+QMTCONN: 0,0,0")) {
                Serial.println(F("[NB-IoT] Autentificat pe Live Objects!"));
                Serial.println(F("[NB-IoT] Abonare la topic comenzi..."));

                s_initState = INIT_MQTSUB;
                s_initStepMs = now;
                s_initStepTimeout = 10000;
                s_stepRetries = 0;

                sendQmtSub();
            } else if (atReadCheck("+QMTCONN: 0,0,4")) {
                Serial.println(F("[NB-IoT] EROARE: username/parola gresite."));
                beginFullMqttReconnect(F("QMTCONN bad credentials"));
            } else if (atReadCheck("+QMTCONN: 0,0,5")) {
                Serial.println(F("[NB-IoT] EROARE: not authorized. Verifica API key / DEVICE_ACCESS."));
                beginFullMqttReconnect(F("QMTCONN not authorized"));
            } else if (strstr(s_atBuf, "+QMTCONN: 0,")) {
                Serial.println(F("[NB-IoT] EROARE QMTCONN:"));
                Serial.println(s_atBuf);
                beginFullMqttReconnect(F("QMTCONN result error"));
            } else if (atHasError()) {
                Serial.println(F("[NB-IoT] EROARE QMTCONN:"));
                Serial.println(s_atBuf);
                beginFullMqttReconnect(F("QMTCONN ERROR"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;

                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB-IoT] EROARE: autentificare MQTT esuata dupa 3 incercari"));
                    printRawIfAny();
                    beginFullMqttReconnect(F("QMTCONN timeout"));
                } else {
                    Serial.print(F("[NB-IoT] Reincercare MQTT auth "));
                    Serial.print(s_stepRetries);
                    Serial.println(F("/3..."));

                    s_initStepMs = now;
                    sendQmtConn();
                }
            }
            break;


        case INIT_MQTSUB:
            if (atReadCheck("+QMTSUB:")) {
                s_connected = true;
                s_initState = INIT_DONE;
                s_mqttReconnectAttempts = 0;

                Serial.println(F("[NB-IoT] === ONLINE pe Live Objects MQTT! ==="));

                Serial.print(F("[NB-IoT] Publish topic: "));
                Serial.println(LO_TOPIC_PUB);

                Serial.print(F("[NB-IoT] Subscribe topic: "));
                Serial.println(LO_TOPIC_SUB);

                nbiot_publish(EVT_SYSTEM_STATUS, "", "BOOT");
            } else if (atHasError()) {
                Serial.println(F("[NB-IoT] EROARE QMTSUB:"));
                Serial.println(s_atBuf);

                beginFullMqttReconnect(F("QMTSUB ERROR"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;

                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB-IoT] EROARE: abonare MQTT esuata dupa 3 incercari"));
                    printRawIfAny();
                    beginFullMqttReconnect(F("QMTSUB timeout"));
                } else {
                    Serial.print(F("[NB-IoT] Reincercare MQTT subscribe "));
                    Serial.print(s_stepRetries);
                    Serial.println(F("/3..."));

                    s_initStepMs = now;
                    sendQmtSub();
                }
            }
            break;


        // ====================================================
        //  Deconectare completă MQTT înainte de reconnect
        // ====================================================

        case INIT_MQTT_UNSUB:
            if (atReadCheck("+QMTUNS:") ||
                atReadCheck("OK") ||
                atHasError() ||
                now - s_initStepMs > s_initStepTimeout)
            {
                Serial.println(F("[NB-IoT] MQTT unsubscribe terminat/ignorat."));
                Serial.println(F("[NB-IoT] Deconectez sesiunea MQTT..."));

                s_initState = INIT_MQTT_DISC;
                s_initStepMs = now;
                s_initStepTimeout = 3000;
                s_stepRetries = 0;

                sendQmtDisc();
            }
            break;


        case INIT_MQTT_DISC:
            if (atReadCheck("+QMTDISC:") ||
                atReadCheck("OK") ||
                atHasError() ||
                now - s_initStepMs > s_initStepTimeout)
            {
                Serial.println(F("[NB-IoT] MQTT disconnect terminat/ignorat."));
                Serial.println(F("[NB-IoT] Inchid socket-ul MQTT..."));

                s_initState = INIT_MQTT_CLOSE;
                s_initStepMs = now;
                s_initStepTimeout = 5000;
                s_stepRetries = 0;

                sendQmtClose();
            }
            break;


        case INIT_MQTT_CLOSE:
            if (atReadCheck("+QMTCLOSE:") ||
                atReadCheck("OK") ||
                atHasError() ||
                now - s_initStepMs > s_initStepTimeout)
            {
                Serial.println(F("[NB-IoT] MQTT socket inchis/ignorat."));
                Serial.println(F("[NB-IoT] Cooldown 5 secunde inainte de reconectare..."));

                s_connected = false;

                s_initState = INIT_RECONNECT_COOLDOWN;
                s_initStepMs = now;
                s_initStepTimeout = MQTT_RECONNECT_COOLDOWN_MS;
                s_stepRetries = 0;
            }
            break;


        case INIT_RECONNECT_COOLDOWN:
            if (now - s_initStepMs >= MQTT_RECONNECT_COOLDOWN_MS) {
                Serial.println(F("[NB-IoT] Reiau procesul complet de conectare..."));

                s_connected = false;
                s_network_ready = false;

                s_atRetries = 0;
                s_stepRetries = 0;
                s_ceregRetries = 0;

                s_initState = INIT_AT_TEST;
                s_initStepMs = now;
                s_initStepTimeout = 5000;

                atWrite("AT");
            }
            break;


        default:
            break;
    }
}


// ============================================================
//  Publicare MQTT / SMS fallback
// ============================================================

void nbiot_publish(const char* eventType,
                   const char* uid,
                   const char* stateStr)
{
    if (s_initState != INIT_DONE && s_initState != INIT_FAILED) {
        Serial.print(F("[NB-IoT] Init in curs, eveniment ignorat: "));
        Serial.println(eventType);
        return;
    }

    if (s_connected) {
        char payload[150];

        snprintf(payload, sizeof(payload),
                 "{\"device_id\":\"%s\",\"event\":\"%s\",\"uid\":\"%s\",\"state\":\"%s\"}",
                 LO_DEVICE_ID,
                 eventType ? eventType : "",
                 uid ? uid : "",
                 stateStr ? stateStr : "");

        int len = strlen(payload);

        clearAtBuffer();

        s_modem.print(F("AT+QMTPUB=0,0,0,0,\""));
        s_modem.print(LO_TOPIC_PUB);
        s_modem.print(F("\","));
        s_modem.println(len);

        Serial.print(F("[AT>>] AT+QMTPUB=0,0,0,0,\""));
        Serial.print(LO_TOPIC_PUB);
        Serial.print(F("\","));
        Serial.println(len);

        bool gotPrompt = waitPrompt(5000);

        if (!gotPrompt) {
            Serial.println(F("[MQTT] EROARE: nu am primit prompt > pentru payload"));
            smsSend(eventType, uid, stateStr);
            return;
        }

        delay(100);

        s_modem.print(payload);

        Serial.print(F("[MQTT] Payload: "));
        Serial.println(payload);

        char buf[140];
        byte pos = 0;
        memset(buf, 0, sizeof(buf));

        unsigned long start = millis();

        while (millis() - start < 10000) {
            while (s_modem.available() && pos < sizeof(buf) - 1) {
                char c = s_modem.read();

                if (c == '\r') continue;

                buf[pos++] = c;
                buf[pos] = '\0';
            }

            if (strstr(buf, "+QMTPUB: 0,0,0")) {
                Serial.println(F("[MQTT] Publicat cu succes!"));
                return;
            }

            if (strstr(buf, "+QMTPUB:")) {
                Serial.print(F("[MQTT] Raspuns publish: "));
                Serial.println(buf);
                return;
            }

            if (strstr(buf, "ERROR") ||
                strstr(buf, "+CME ERROR") ||
                strstr(buf, "+CMS ERROR"))
            {
                Serial.print(F("[MQTT] ERROR publish: "));
                Serial.println(buf);

                s_connected = false;
                beginFullMqttReconnect(F("QMTPUB ERROR"));

                smsSend(eventType, uid, stateStr);
                return;
            }
        }

        Serial.println(F("[MQTT] Timeout publish"));

        s_connected = false;
        beginFullMqttReconnect(F("QMTPUB timeout"));

        smsSend(eventType, uid, stateStr);
        return;
    }

    bool sendSms =
        strcmp(eventType, EVT_ALARM_START)   == 0 ||
        strcmp(eventType, EVT_ACCESS_DENIED) == 0 ||
        strcmp(eventType, EVT_REMOTE_OPEN)   == 0 ||
        strcmp(eventType, EVT_SYSTEM_STATUS) == 0;

    if (!sendSms) {
        Serial.print(F("[NB-IoT] Offline, ignorat: "));
        Serial.println(eventType);
        return;
    }

    if (!s_network_ready) {
        Serial.println(F("[SMS] Retea indisponibila, SMS imposibil"));
        return;
    }

    smsSend(eventType, uid, stateStr);
}


// ============================================================
//  Parsare JSON comenzi
// ============================================================

static bool parseJsonField(const char* json,
                           const char* field,
                           char* out,
                           byte outSize)
{
    char needle[24];

    snprintf(needle, sizeof(needle), "\"%s\":\"", field);

    const char* start = strstr(json, needle);

    if (!start) return false;

    start += strlen(needle);

    const char* end = strchr(start, '"');

    if (!end) return false;

    byte len = (byte)(end - start);

    if (len >= outSize) {
        len = outSize - 1;
    }

    strncpy(out, start, len);
    out[len] = '\0';

    return true;
}


// ============================================================
//  Comenzi primite MQTT
// ============================================================

void nbiot_checkCommands() {
    if (!s_connected || !s_cmdCallback) return;

    while (s_modem.available() && s_mqttPos < sizeof(s_mqttBuf) - 1) {
        char c = s_modem.read();

        if (c == '\r') continue;

        s_mqttBuf[s_mqttPos++] = c;
        s_mqttBuf[s_mqttPos] = '\0';
    }

    if (!strstr(s_mqttBuf, "+QMTRECV:")) return;
    if (!strchr(s_mqttBuf, '\n')) return;

    Serial.print(F("[MQTT] Comanda primita: "));
    Serial.println(s_mqttBuf);

    const char* jsonStart = strchr(s_mqttBuf, '{');

    if (!jsonStart) {
        s_mqttPos = 0;
        memset(s_mqttBuf, 0, sizeof(s_mqttBuf));
        return;
    }

    char jsonBuf[140];
    strncpy(jsonBuf, jsonStart, sizeof(jsonBuf) - 1);
    jsonBuf[sizeof(jsonBuf) - 1] = '\0';

    s_mqttPos = 0;
    memset(s_mqttBuf, 0, sizeof(s_mqttBuf));

    char* jsonEnd = strchr(jsonBuf, '}');

    if (jsonEnd) {
        *(jsonEnd + 1) = '\0';
    }

    char cmdStr[18];

    if (!parseJsonField(jsonBuf, "cmd", cmdStr, sizeof(cmdStr))) {
        return;
    }

    NbiotCommand command;
    memset(&command, 0, sizeof(command));
    command.type = CMD_NONE;

    if (strcmp(cmdStr, "open") == 0) {
        command.type = CMD_OPEN;
    } else if (strcmp(cmdStr, "card_add") == 0) {
        command.type = CMD_CARD_ADD;

        if (!parseJsonField(jsonBuf, "uid", command.uid, sizeof(command.uid))) {
            return;
        }
    } else if (strcmp(cmdStr, "card_remove") == 0) {
        command.type = CMD_CARD_REMOVE;

        if (!parseJsonField(jsonBuf, "uid", command.uid, sizeof(command.uid))) {
            return;
        }
    } else if (strcmp(cmdStr, "status") == 0) {
        command.type = CMD_STATUS;
    } else {
        return;
    }

    s_cmdCallback(command);
}


// ============================================================
//  Callback / heartbeat / status
// ============================================================

void nbiot_setCommandCallback(NbiotCommandCallback cb) {
    s_cmdCallback = cb;
}

void nbiot_heartbeatTick(const char* stateStr) {
    if (!s_connected) return;

    unsigned long now = millis();

    if (now - s_lastHeartbeatMs >= NBIOT_HEARTBEAT_MS) {
        s_lastHeartbeatMs = now;
        nbiot_publish(EVT_SYSTEM_STATUS, "", stateStr);
    }
}

bool nbiot_isConnected() {
    return s_connected;
}