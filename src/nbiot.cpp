#include "nbiot.h"
#include "config.h"
#include <SoftwareSerial.h>

// ============================================================
//  nbiot.cpp — BC92 + MQTT Live Objects + fallback SMS
//  OPTIMIZAT AGRESIV PENTRU COBORÂREA CONSUMULUI DE RAM (<66%)
// ============================================================

#define MODEM_BAUD  9600
static SoftwareSerial s_modem(MODEM_RX_PIN, MODEM_TX_PIN);

enum InitState {
    INIT_IDLE,
    INIT_AT_TEST,
    INIT_ECHO_OFF,
    INIT_CEREG,
    INIT_MQTCFG,
    INIT_MQTOPEN,
    INIT_MQTCONN,
    INIT_MQTSUB,
    INIT_DONE,
    INIT_FAILED
};

static InitState     s_initState       = INIT_IDLE;
static unsigned long s_initStepMs      = 0;
static unsigned long s_initStepTimeout = 0;
static byte          s_ceregRetries    = 0;
static byte          s_atRetries       = 0;

static bool                  s_connected       = false;
static NbiotCommandCallback  s_cmdCallback     = nullptr;
static unsigned long         s_lastHeartbeatMs = 0;

// !!! OPTIMIZARE CRITICĂ: Buffere reduse de la 200 la 90 de octeți !!!
static char s_atBuf[90];
static byte s_atPos = 0;

static char s_mqttBuf[90];
static byte s_mqttPos = 0;

static void atWrite(const char* cmd) {
    while (s_modem.available()) s_modem.read();
    s_atPos = 0;
    memset(s_atBuf, 0, sizeof(s_atBuf));
    s_modem.println(cmd);
    Serial.print(F("[AT>>] "));
    Serial.println(cmd);
}

static bool atReadCheck(const char* expect) {
    while (s_modem.available() && s_atPos < sizeof(s_atBuf) - 1) {
        s_atBuf[s_atPos++] = s_modem.read();
        s_atBuf[s_atPos]   = '\0';
    }
    if (strstr(s_atBuf, expect)) {
        Serial.print(F("[AT<<] "));
        Serial.println(s_atBuf);
        return true;
    }
    return false;
}

// !!! OPTIMIZARE CRITICĂ: Buffer local redus de la 300 la 90 de octeți !!!
static bool atSendBlocking(const char* cmd, const char* expect, unsigned int timeoutMs) {
    while (s_modem.available()) s_modem.read();
    s_modem.println(cmd);
    char buf[90]; 
    byte pos = 0;
    memset(buf, 0, sizeof(buf));
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        while (s_modem.available() && pos < sizeof(buf) - 1) {
            buf[pos++] = s_modem.read();
            buf[pos]   = '\0';
            if (strstr(buf, expect)) {
                Serial.print(F("[AT<<] "));
                Serial.println(buf);
                return true;
            }
        }
    }
    Serial.println(F("[AT] timeout blocker"));
    return false;
}

static void smsSend(const char* message) {
    Serial.print(F("[SMS] Trimit catre "));
    Serial.print(LO_SMS_ADMIN_NR);
    Serial.print(F(": "));
    Serial.println(message);

    if (!atSendBlocking("AT+CMGF=1", "OK", 2000)) {
        Serial.println(F("[SMS] EROARE: AT+CMGF=1 esuat"));
        return;
    }

    // !!! OPTIMIZARE: Refolosim un buffer local mic !!!
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", LO_SMS_ADMIN_NR);
    if (!atSendBlocking(cmd, ">", 3000)) {
        Serial.println(F("[SMS] EROARE: numar destinatar esuat"));
        return;
    }

    s_modem.print(message);
    s_modem.write(0x1A);

    // !!! OPTIMIZARE: Buffer local pentru SMS redus la 45 de octeți !!!
    char buf[45];
    byte pos = 0;
    memset(buf, 0, sizeof(buf));
    unsigned long start = millis();
    while (millis() - start < 10000) {
        while (s_modem.available() && pos < sizeof(buf) - 1) {
            buf[pos++] = s_modem.read();
            buf[pos]   = '\0';
        }
        if (strstr(buf, "+CMGS:")) {
            Serial.println(F("[SMS] Trimis cu succes!"));
            return;
        }
        if (strstr(buf, "ERROR")) {
            Serial.println(F("[SMS] EROARE la trimitere"));
            return;
        }
    }
    Serial.println(F("[SMS] timeout trimitere"));
}

bool nbiot_init() {
    s_modem.begin(MODEM_BAUD);
    delay(500);

    Serial.println(F("[NB-IoT] Pornire initializare non-blocanta..."));
    Serial.println(F("[NB-IoT] RFID/senzori functioneaza imediat."));
    Serial.println(F("[NB-IoT] Conectare cloud in background..."));

    s_initState       = INIT_AT_TEST;
    s_initStepMs      = millis();
    s_initStepTimeout = 5000;  
    s_ceregRetries    = 0;
    s_atRetries       = 0;

    atWrite("AT");
    return true;
}

void nbiot_initTick() {
    if (s_initState == INIT_DONE || s_initState == INIT_FAILED) return;
    if (s_initState == INIT_IDLE) return;

    unsigned long now = millis();

    switch (s_initState) {

        case INIT_AT_TEST:
            if (atReadCheck("OK")) {
                Serial.println(F("[NB-IoT] Modem raspunde!"));
                s_initState       = INIT_ECHO_OFF;
                s_initStepMs      = now;
                s_initStepTimeout = 2000;
                atWrite("ATE0");
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_atRetries++;
                if (s_atRetries >= 5) {
                    Serial.println(F("[NB-IoT] EROARE: modem nu raspunde dupa 5 incercari"));
                    Serial.println(F("[NB-IoT] Verifica: TX(D2)->RX(BC92), RX(D3)->TX(BC92)"));
                    Serial.println(F("[NB-IoT] Sistem functioneaza LOCAL. SMS/MQTT indisponibil."));
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
            if (atReadCheck("OK") || now - s_initStepMs > 2000) {
                s_initState       = INIT_CEREG;
                s_initStepMs      = now;
                s_initStepTimeout = 5000;
                s_ceregRetries    = 0;
                Serial.println(F("[NB-IoT] Astept inregistrare retea NB-IoT..."));
                atWrite("AT+CEREG?");
            }
            break;

        case INIT_CEREG:
            if (atReadCheck("+CEREG: 0,1") || atReadCheck("+CEREG: 0,5")) {
                Serial.println(F("[NB-IoT] Inregistrat in retea!"));
                s_initState       = INIT_MQTCFG;
                s_initStepMs      = now;
                s_initStepTimeout = 2000;
                atWrite("AT+QMTCFG=\"recv/mode\",0,0,1");
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_ceregRetries++;
                if (s_ceregRetries >= 12) {  
                    Serial.println(F("[NB-IoT] EROARE: retea indisponibila (no coverage)"));
                    s_initState = INIT_FAILED;
                } else {
                    s_initStepMs = now;
                    atWrite("AT+CEREG?");
                }
            }
            break;

        case INIT_MQTCFG:
            if (atReadCheck("OK") || now - s_initStepMs > s_initStepTimeout) {
                s_initState       = INIT_MQTOPEN;
                s_initStepMs      = now;
                s_initStepTimeout = 15000;
                
                // !!! OPTIMIZARE: Generăm stringul direct din Flash economisind stiva !!!
                s_modem.print(F("AT+QMTOPEN=0,\""));
                s_modem.print(LO_MQTT_HOST);
                s_modem.print(F("\","));
                s_modem.println(LO_MQTT_PORT);
                
                s_atPos = 0;
                memset(s_atBuf, 0, sizeof(s_atBuf));
                
                Serial.println(F("[NB-IoT] Conectare TCP broker MQTT..."));
            }
            break;

        case INIT_MQTOPEN:
            if (atReadCheck("+QMTOPEN: 0,0")) {
                Serial.println(F("[NB-IoT] TCP conectat!"));
                s_initState       = INIT_MQTCONN;
                s_initStepMs      = now;
                s_initStepTimeout = 10000;
                
                // !!! OPTIMIZARE MASTER: Eliminăm bufferul uriaș de 200 bytes din stivă. Trimitem direct bucăți din Flash !!!
                s_modem.print(F("AT+QMTCONN=0,\""));
                s_modem.print(LO_MQTT_CLIENT);
                s_modem.print(F("\",\""));
                s_modem.print(LO_MQTT_USER);
                s_modem.print(F("\",\""));
                s_modem.print(LO_API_KEY);
                s_modem.println(F("\""));
                
                s_atPos = 0;
                memset(s_atBuf, 0, sizeof(s_atBuf));
                
                Serial.println(F("[NB-IoT] Autentificare MQTT..."));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                Serial.println(F("[NB-IoT] EROARE: TCP open timeout"));
                s_initState = INIT_FAILED;
            }
            break;

        case INIT_MQTCONN:
            if (atReadCheck("+QMTCONN: 0,0,0")) {
                Serial.println(F("[NB-IoT] Autentificat pe Live Objects!"));
                s_initState       = INIT_MQTSUB;
                s_initStepMs      = now;
                s_initStepTimeout = 5000;
                
                s_modem.print(F("AT+QMTSUB=0,1,\""));
                s_modem.print(LO_TOPIC_SUB);
                s_modem.println(F("\",1"));
                
                s_atPos = 0;
                memset(s_atBuf, 0, sizeof(s_atBuf));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                Serial.println(F("[NB-IoT] EROARE: autentificare MQTT esuata"));
                s_initState = INIT_FAILED;
            }
            break;

        case INIT_MQTSUB:
            if (atReadCheck("+QMTSUB:") || now - s_initStepMs > s_initStepTimeout) {
                s_connected = true;
                s_initState = INIT_DONE;
                Serial.println(F("[NB-IoT] === ONLINE pe Live Objects! ==="));
                nbiot_publish(EVT_SYSTEM_STATUS, "", "BOOT");
            }
            break;

        default:
            break;
    }
}

void nbiot_publish(const char* eventType, const char* uid, const char* stateStr) {
    if (s_connected) {
        // !!! OPTIMIZARE: Trimitem payload-ul JSON fragmentat direct pe serială, salvând 500 de octeți de stivă !!!
        s_modem.print(F("AT+QMTPUB=0,1,0,0,\""));
        s_modem.print(LO_TOPIC_PUB);
        s_modem.print(F("\",\"{\\\"s\\\":\\\""));
        s_modem.print(LO_STREAM_ID);
        s_modem.print(F("\\\",\\\"v\\\":{\\\"event\\\":\\\""));
        s_modem.print(eventType);
        s_modem.print(F("\\\",\\\"uid\\\":\\\""));
        s_modem.print(uid);
        s_modem.print(F("\\\",\\\"state\\\":\\\""));
        s_modem.print(stateStr);
        s_modem.println(F("\\\"}}\""));

        Serial.print(F("[MQTT] Publish direct: "));
        Serial.println(eventType);

        atSendBlocking("", "+QMTPUB:", 3000);
        return;
    }

    bool sendSms = (strcmp(eventType, EVT_ALARM_START)    == 0 ||
                    strcmp(eventType, EVT_ACCESS_DENIED)   == 0 ||
                    strcmp(eventType, EVT_REMOTE_OPEN)     == 0 ||
                    strcmp(eventType, EVT_SYSTEM_STATUS)   == 0);

    if (!sendSms) {
        Serial.print(F("[NB-IoT] Offline, ignorat: "));
        Serial.println(eventType);
        return;
    }

    char sms[100]; // Redus la 100 max
    if (uid[0] != '\0') {
        snprintf(sms, sizeof(sms), "RFID:%s\nUID:%s\nST:%s", eventType, uid, stateStr);
    } else {
        snprintf(sms, sizeof(sms), "RFID:%s\nST:%s", eventType, stateStr);
    }

    if (s_initState == INIT_FAILED) {
        Serial.println(F("[SMS] Modem offline, SMS imposibil"));
        return;
    }

    smsSend(sms);
}

static bool parseJsonField(const char* json, const char* field, char* out, byte outSize) {
    char needle[20];
    snprintf(needle, sizeof(needle), "\"%s\":\"", field);
    const char* start = strstr(json, needle);
    if (!start) return false;
    start += strlen(needle);
    const char* end = strchr(start, '"');
    if (!end) return false;
    byte len = (byte)(end - start);
    if (len >= outSize) len = outSize - 1;
    strncpy(out, start, len);
    out[len] = '\0';
    return true;
}

void nbiot_checkCommands() {
    if (!s_connected || !s_cmdCallback) return;

    while (s_modem.available() && s_mqttPos < sizeof(s_mqttBuf) - 1) {
        s_mqttBuf[s_mqttPos++] = s_modem.read();
        s_mqttBuf[s_mqttPos]   = '\0';
    }

    if (!strstr(s_mqttBuf, "+QMTRECV:")) return;
    if (!strchr(s_mqttBuf, '\n')) return;

    Serial.print(F("[MQTT] Comanda: "));
    Serial.println(s_mqttBuf);

    const char* jsonStart = strchr(s_mqttBuf, '{');
    s_mqttPos = 0;
    memset(s_mqttBuf, 0, sizeof(s_mqttBuf));
    if (!jsonStart) return;

    char jsonBuf[80]; // Redus la un nivel sigur
    strncpy(jsonBuf, jsonStart, sizeof(jsonBuf) - 1);
    jsonBuf[sizeof(jsonBuf) - 1] = '\0';
    char* jsonEnd = strchr(jsonBuf, '}');
    if (jsonEnd) *(jsonEnd + 1) = '\0';

    char cmdStr[15];
    if (!parseJsonField(jsonBuf, "cmd", cmdStr, sizeof(cmdStr))) return;

    NbiotCommand command;
    memset(&command, 0, sizeof(command));

    if (strcmp(cmdStr, "open") == 0) {
        command.type = CMD_OPEN;
    } else if (strcmp(cmdStr, "card_add") == 0) {
        command.type = CMD_CARD_ADD;
        if (!parseJsonField(jsonBuf, "uid", command.uid, sizeof(command.uid))) return;
    } else if (strcmp(cmdStr, "card_remove") == 0) {
        command.type = CMD_CARD_REMOVE;
        if (!parseJsonField(jsonBuf, "uid", command.uid, sizeof(command.uid))) return;
    } else if (strcmp(cmdStr, "status") == 0) {
        command.type = CMD_STATUS;
    } else {
        return;
    }

    s_cmdCallback(command);
}

void nbiot_setCommandCallback(NbiotCommandCallback cb) { s_cmdCallback = cb; }

void nbiot_heartbeatTick(const char* stateStr) {
    if (!s_connected) return;
    unsigned long now = millis();
    if (now - s_lastHeartbeatMs >= NBIOT_HEARTBEAT_MS) {
        s_lastHeartbeatMs = now;
        nbiot_publish(EVT_SYSTEM_STATUS, "", stateStr);
    }
}

bool nbiot_isConnected() { return s_connected; }