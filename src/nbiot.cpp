#include "nbiot.h"
#include "config.h"
#include <SoftwareSerial.h>

#define MODEM_BAUD 9600

#define MQTT_RECONNECT_MAX_ATTEMPTS 5
#define MQTT_RECONNECT_COOLDOWN_MS  5000UL

#define AT_BUF_SIZE      112
#define MQTT_BUF_SIZE    96
#define PAYLOAD_BUF_SIZE 96

static SoftwareSerial s_modem(MODEM_RX_PIN, MODEM_TX_PIN);

enum InitState : byte {
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
static unsigned long s_lastHeartbeatMs = 0;

static byte s_atRetries = 0;
static byte s_stepRetries = 0;
static byte s_ceregRetries = 0;
static byte s_mqttReconnectAttempts = 0;

static bool s_connected = false;
static bool s_network_ready = false;

static NbiotCommandCallback s_cmdCallback = nullptr;

static char s_atBuf[AT_BUF_SIZE];
static byte s_atPos = 0;

static char s_mqttBuf[MQTT_BUF_SIZE];
static byte s_mqttPos = 0;

static void readAtBuffer() {
    while (s_modem.available() && s_atPos < sizeof(s_atBuf) - 1) {
        char c = s_modem.read();
        if (c == '\r') continue;
        s_atBuf[s_atPos++] = c;
        s_atBuf[s_atPos] = '\0';
    }
}

static void clearAtBuffer() {
    while (s_modem.available()) s_modem.read();
    s_atPos = 0;
    s_atBuf[0] = '\0';
}

static void atWrite(const char* cmd) {
    clearAtBuffer();
    s_modem.println(cmd);
    Serial.print(F("[AT>>] "));
    Serial.println(cmd);
    delay(80);
}

static void atWrite(const __FlashStringHelper* cmd) {
    clearAtBuffer();
    s_modem.println(cmd);
    Serial.print(F("[AT>>] "));
    Serial.println(cmd);
    delay(80);
}

static bool atContains(const char* text) {
    readAtBuffer();
    return strstr(s_atBuf, text) != nullptr;
}

static bool atReadCheck(const char* expect) {
    delay(40);
    readAtBuffer();
    if (strstr(s_atBuf, expect)) {
        Serial.print(F("[AT<<] "));
        Serial.println(s_atBuf);
        return true;
    }
    return false;
}

static bool atHasError() {
    readAtBuffer();
    return strstr(s_atBuf, "ERROR") != nullptr ||
           strstr(s_atBuf, "+CME ERROR") != nullptr ||
           strstr(s_atBuf, "+CMS ERROR") != nullptr;
}

static void printRawIfAny() {
    readAtBuffer();
    if (s_atPos > 0) {
        Serial.print(F("[AT RAW] "));
        Serial.println(s_atBuf);
    }
}

static bool atSendBlocking(const char* cmd,
                           const char* expect,
                           unsigned long timeoutMs)
{
    while (s_modem.available()) s_modem.read();

    if (cmd && cmd[0]) {
        s_modem.println(cmd);
        Serial.print(F("[AT>>] "));
        Serial.println(cmd);
    }

    char buf[80];
    byte pos = 0;
    buf[0] = '\0';

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

    Serial.println(F("[AT] timeout"));
    return false;
}

static bool waitPrompt(unsigned long timeoutMs) {
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        while (s_modem.available()) {
            if (s_modem.read() == '>') return true;
        }
    }
    return false;
}

static int parseCsqRssi() {
    const char* p = strstr(s_atBuf, "+CSQ:");
    if (!p) return -1;

    p += 5;
    while (*p == ' ') p++;
    return atoi(p);
}

static void sendQmtOpen() {
    clearAtBuffer();

    s_modem.print(F("AT+QMTOPEN=0,\""));
    s_modem.print(LO_MQTT_HOST);
    s_modem.print(F("\","));
    s_modem.print(LO_MQTT_PORT);
    s_modem.println();

    Serial.print(F("[AT>>] QMTOPEN "));
    Serial.println(LO_MQTT_HOST);
    delay(80);
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

    Serial.println(F("[AT>>] QMTCONN ***"));
    delay(80);
}

static void sendQmtSub() {
    clearAtBuffer();

    s_modem.print(F("AT+QMTSUB=0,1,\""));
    s_modem.print(LO_TOPIC_SUB);
    s_modem.println(F("\",1"));

    Serial.print(F("[AT>>] QMTSUB "));
    Serial.println(LO_TOPIC_SUB);
    delay(80);
}

static void sendQmtUnsub() {
    clearAtBuffer();

    s_modem.print(F("AT+QMTUNS=0,2,\""));
    s_modem.print(LO_TOPIC_SUB);
    s_modem.println(F("\""));

    Serial.println(F("[AT>>] QMTUNS"));
    delay(80);
}

static void sendQmtDisc() {
    clearAtBuffer();
    s_modem.println(F("AT+QMTDISC=0"));
    Serial.println(F("[AT>>] QMTDISC"));
    delay(80);
}

static void sendQmtClose() {
    clearAtBuffer();
    s_modem.println(F("AT+QMTCLOSE=0"));
    Serial.println(F("[AT>>] QMTCLOSE"));
    delay(80);
}

static void beginFullMqttReconnect(const __FlashStringHelper* reason) {
    Serial.print(F("[NB] Reconnect: "));
    Serial.println(reason);

    s_connected = false;

    if (s_mqttReconnectAttempts >= MQTT_RECONNECT_MAX_ATTEMPTS) {
        Serial.println(F("[NB] MQTT failed. Local mode."));
        s_initState = INIT_FAILED;
        return;
    }

    s_mqttReconnectAttempts++;
    Serial.print(F("[NB] Retry "));
    Serial.print(s_mqttReconnectAttempts);
    Serial.print('/');
    Serial.println(MQTT_RECONNECT_MAX_ATTEMPTS);

    s_stepRetries = 0;
    s_initStepMs = millis();
    s_initStepTimeout = 2500;
    s_initState = INIT_MQTT_UNSUB;
    sendQmtUnsub();
}

static void smsSend(const char* eventType,
                    const char* uid,
                    const char* stateStr)
{
    Serial.print(F("[SMS] "));
    Serial.println(eventType);

    atSendBlocking("AT+CGSMS=2", "OK", 1200);
    atSendBlocking("AT+CMGF=1", "OK", 1800);

    char cmd[28];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", LO_SMS_ADMIN_NR);

    if (!atSendBlocking(cmd, ">", 4500)) {
        Serial.println(F("[SMS] no prompt"));
        s_modem.write(27);
        return;
    }

    s_modem.print(F("EVT:"));
    s_modem.print(eventType);
    s_modem.print(F(",ST:"));
    s_modem.print(stateStr ? stateStr : "");

    if (uid && uid[0]) {
        s_modem.print(F(",UID:"));
        s_modem.print(uid);
    }

    delay(200);
    s_modem.write(26);

    char buf[64];
    byte pos = 0;
    buf[0] = '\0';

    unsigned long start = millis();
    while (millis() - start < 10000) {
        while (s_modem.available() && pos < sizeof(buf) - 1) {
            char c = s_modem.read();
            if (c == '\r') continue;

            buf[pos++] = c;
            buf[pos] = '\0';
        }

        if (strstr(buf, "+CMGS:") || strstr(buf, "OK")) {
            Serial.println(F("[SMS] OK"));
            return;
        }

        if (strstr(buf, "ERROR")) {
            Serial.print(F("[SMS] ERR "));
            Serial.println(buf);
            return;
        }
    }

    Serial.println(F("[SMS] timeout"));
}

bool nbiot_init() {
    s_modem.begin(MODEM_BAUD);
    delay(600);

    Serial.println(F("[NB] init background"));

    s_connected = false;
    s_network_ready = false;

    s_atRetries = 0;
    s_stepRetries = 0;
    s_ceregRetries = 0;
    s_mqttReconnectAttempts = 0;

    s_atPos = 0;
    s_mqttPos = 0;
    s_atBuf[0] = '\0';
    s_mqttBuf[0] = '\0';

    s_initState = INIT_AT_TEST;
    s_initStepMs = millis();
    s_initStepTimeout = 5000;

    atWrite("AT");
    return true;
}

void nbiot_initTick() {
    if (s_initState == INIT_DONE || s_initState == INIT_FAILED || s_initState == INIT_IDLE) return;

    unsigned long now = millis();

    switch (s_initState) {
        case INIT_AT_TEST:
            if (atReadCheck("OK")) {
                Serial.println(F("[NB] modem OK"));
                s_initState = INIT_ECHO_OFF;
                s_initStepMs = now;
                s_initStepTimeout = 3000;
                s_stepRetries = 0;
                atWrite("ATE0");
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_atRetries++;
                if (s_atRetries >= 5) {
                    Serial.println(F("[NB] modem offline"));
                    s_initState = INIT_FAILED;
                } else {
                    s_initStepMs = now;
                    atWrite("AT");
                }
            }
            break;

        case INIT_ECHO_OFF:
            if (atReadCheck("OK")) {
                Serial.println(F("[NB] echo off"));
                s_initState = INIT_APN;
                s_initStepMs = now;
                s_initStepTimeout = 10000;
                s_stepRetries = 0;
                atWrite(F("AT+QCGDEFCONT=\"IP\",\"net\""));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB] ATE0 fail"));
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
                Serial.println(F("[NB] APN OK"));
                s_initState = INIT_CEREG;
                s_initStepMs = now;
                s_initStepTimeout = 5000;
                s_ceregRetries = 0;
                atWrite(F("AT+CEREG?"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB] APN fail"));
                    printRawIfAny();
                    s_initState = INIT_FAILED;
                } else {
                    s_initStepMs = now;
                    atWrite(F("AT+QCGDEFCONT=\"IP\",\"net\""));
                }
            }
            break;

        case INIT_CEREG:
            if (atReadCheck("+CEREG: 0,1") ||
                atContains("+CEREG: 0,5") ||
                atContains("+CEREG: 1,1") ||
                atContains("+CEREG: 1,5"))
            {
                Serial.println(F("[NB] network OK"));
                s_network_ready = true;

                s_initState = INIT_CSQ;
                s_initStepMs = now;
                s_initStepTimeout = 5000;
                s_stepRetries = 0;
                atWrite(F("AT+CSQ"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_ceregRetries++;
                if (s_ceregRetries >= 12) {
                    Serial.println(F("[NB] no network"));
                    printRawIfAny();
                    s_initState = INIT_FAILED;
                } else {
                    s_initStepMs = now;
                    atWrite(F("AT+CEREG?"));
                }
            }
            break;

        case INIT_CSQ:
            if (atReadCheck("OK")) {
                int rssi = parseCsqRssi();
                Serial.print(F("[NB] CSQ="));
                Serial.println(rssi);

                if (rssi == 99 || rssi < 5) {
                    s_stepRetries++;
                    if (s_stepRetries >= 5) {
                        Serial.println(F("[NB] weak signal"));
                        s_initState = INIT_FAILED;
                    } else {
                        s_initStepMs = now;
                        atWrite(F("AT+CSQ"));
                    }
                } else {
                    s_initState = INIT_MQTT_VERSION;
                    s_initStepMs = now;
                    s_initStepTimeout = 5000;
                    s_stepRetries = 0;
                    atWrite(F("AT+QMTCFG=\"version\",0,1"));
                }
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 5) {
                    Serial.println(F("[NB] CSQ fail"));
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
                Serial.println(F("[NB] MQTT 3.1.1"));
                s_initState = INIT_MQTT_SSL;
                s_initStepMs = now;
                s_initStepTimeout = 5000;
                s_stepRetries = 0;
                atWrite(F("AT+QMTCFG=\"ssl\",0,1,1,0"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) {
                    printRawIfAny();
                    beginFullMqttReconnect(F("version"));
                } else {
                    s_initStepMs = now;
                    atWrite(F("AT+QMTCFG=\"version\",0,1"));
                }
            }
            break;

        case INIT_MQTT_SSL:
            if (atReadCheck("OK")) {
                Serial.println(F("[NB] SSL OK"));
                s_initState = INIT_MQTOPEN;
                s_initStepMs = now;
                s_initStepTimeout = 30000;
                s_stepRetries = 0;
                sendQmtOpen();
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) {
                    printRawIfAny();
                    beginFullMqttReconnect(F("ssl"));
                } else {
                    s_initStepMs = now;
                    atWrite(F("AT+QMTCFG=\"ssl\",0,1,1,0"));
                }
            }
            break;

        case INIT_MQTOPEN:
            if (atReadCheck("+QMTOPEN: 0,0")) {
                Serial.println(F("[NB] QMTOPEN OK"));
                s_initState = INIT_MQTCONN;
                s_initStepMs = now;
                s_initStepTimeout = 30000;
                s_stepRetries = 0;
                sendQmtConn();
            } else if ((atContains("+QMTOPEN: 0,") && !strstr(s_atBuf, "+QMTOPEN: 0,0")) || atHasError()) {
                Serial.println(F("[NB] QMTOPEN fail"));
                printRawIfAny();
                beginFullMqttReconnect(F("QMTOPEN"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                Serial.println(F("[NB] QMTOPEN timeout"));
                printRawIfAny();
                beginFullMqttReconnect(F("QMTOPEN timeout"));
            }
            break;

        case INIT_MQTCONN:
            if (atReadCheck("+QMTCONN: 0,0,0")) {
                Serial.println(F("[NB] MQTT auth OK"));
                s_initState = INIT_MQTSUB;
                s_initStepMs = now;
                s_initStepTimeout = 10000;
                s_stepRetries = 0;
                sendQmtSub();
            } else if (atContains("+QMTCONN: 0,0,4") ||
                       atContains("+QMTCONN: 0,0,5") ||
                       atContains("+QMTCONN: 0,1") ||
                       atContains("+QMTCONN: 0,2") ||
                       atContains("+QMTCONN: 0,3") ||
                       atContains("+QMTCONN: 0,4") ||
                       atContains("+QMTCONN: 0,5") ||
                       atHasError()) {
                Serial.println(F("[NB] QMTCONN fail"));
                printRawIfAny();
                beginFullMqttReconnect(F("QMTCONN"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB] QMTCONN timeout"));
                    printRawIfAny();
                    beginFullMqttReconnect(F("QMTCONN timeout"));
                } else {
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
                Serial.println(F("[NB] ONLINE MQTT"));
                nbiot_publish(EVT_SYSTEM_STATUS, "", "BOOT");
            } else if (atHasError()) {
                Serial.println(F("[NB] QMTSUB fail"));
                printRawIfAny();
                beginFullMqttReconnect(F("QMTSUB"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB] QMTSUB timeout"));
                    printRawIfAny();
                    beginFullMqttReconnect(F("QMTSUB timeout"));
                } else {
                    s_initStepMs = now;
                    sendQmtSub();
                }
            }
            break;

        case INIT_MQTT_UNSUB:
            if (atReadCheck("+QMTUNS:") || atContains("OK") || atHasError() ||
                now - s_initStepMs > s_initStepTimeout)
            {
                s_initState = INIT_MQTT_DISC;
                s_initStepMs = now;
                s_initStepTimeout = 2500;
                sendQmtDisc();
            }
            break;

        case INIT_MQTT_DISC:
            if (atReadCheck("+QMTDISC:") || atContains("OK") || atHasError() ||
                now - s_initStepMs > s_initStepTimeout)
            {
                s_initState = INIT_MQTT_CLOSE;
                s_initStepMs = now;
                s_initStepTimeout = 4000;
                sendQmtClose();
            }
            break;

        case INIT_MQTT_CLOSE:
            if (atReadCheck("+QMTCLOSE:") || atContains("OK") || atHasError() ||
                now - s_initStepMs > s_initStepTimeout)
            {
                Serial.println(F("[NB] reconnect cooldown"));
                s_connected = false;
                s_initState = INIT_RECONNECT_COOLDOWN;
                s_initStepMs = now;
                s_initStepTimeout = MQTT_RECONNECT_COOLDOWN_MS;
            }
            break;

        case INIT_RECONNECT_COOLDOWN:
            if (now - s_initStepMs >= MQTT_RECONNECT_COOLDOWN_MS) {
                Serial.println(F("[NB] reconnect start"));

                s_connected = false;
                s_network_ready = false;
                s_atRetries = 0;
                s_stepRetries = 0;
                s_ceregRetries = 0;
                s_atPos = 0;
                s_mqttPos = 0;
                s_atBuf[0] = '\0';
                s_mqttBuf[0] = '\0';

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

void nbiot_publish(const char* eventType,
                   const char* uid,
                   const char* stateStr)
{
    if (s_initState != INIT_DONE && s_initState != INIT_FAILED) {
        Serial.print(F("[NB] init, drop "));
        Serial.println(eventType);
        return;
    }

    if (s_connected) {
        char payload[PAYLOAD_BUF_SIZE];

        int written = snprintf(payload, sizeof(payload),
                               "{\"d\":\"%s\",\"e\":\"%s\",\"u\":\"%s\",\"s\":\"%s\"}",
                               LO_DEVICE_ID,
                               eventType ? eventType : "",
                               uid ? uid : "",
                               stateStr ? stateStr : "");

        if (written <= 0 || written >= (int)sizeof(payload)) {
            Serial.println(F("[MQTT] payload too big"));
            return;
        }

        clearAtBuffer();

        s_modem.print(F("AT+QMTPUB=0,0,0,0,\""));
        s_modem.print(LO_TOPIC_PUB);
        s_modem.print(F("\","));
        s_modem.println(written);

        Serial.print(F("[AT>>] PUB len="));
        Serial.println(written);

        if (!waitPrompt(5000)) {
            Serial.println(F("[MQTT] no prompt"));
            beginFullMqttReconnect(F("PUB no prompt"));
            return;
        }

        delay(80);
        s_modem.print(payload);

        char buf[72];
        byte pos = 0;
        buf[0] = '\0';

        unsigned long start = millis();
        while (millis() - start < 10000) {
            while (s_modem.available() && pos < sizeof(buf) - 1) {
                char c = s_modem.read();
                if (c == '\r') continue;

                buf[pos++] = c;
                buf[pos] = '\0';
            }

            if (strstr(buf, "+QMTPUB: 0,0,0")) {
                Serial.println(F("[MQTT] PUB OK"));
                return;
            }

            if (strstr(buf, "+QMTPUB:")) {
                Serial.print(F("[MQTT] PUB resp "));
                Serial.println(buf);
                return;
            }

            if (strstr(buf, "ERROR")) {
                Serial.print(F("[MQTT] PUB err "));
                Serial.println(buf);
                beginFullMqttReconnect(F("PUB error"));
                return;
            }
        }

        Serial.println(F("[MQTT] PUB timeout"));
        beginFullMqttReconnect(F("PUB timeout"));
        return;
    }

    bool sendSms =
        strcmp(eventType, EVT_ALARM_START)   == 0 ||
        strcmp(eventType, EVT_ACCESS_DENIED) == 0 ||
        strcmp(eventType, EVT_REMOTE_OPEN)   == 0 ||
        strcmp(eventType, EVT_SYSTEM_STATUS) == 0;

    if (!sendSms) {
        Serial.print(F("[NB] offline drop "));
        Serial.println(eventType);
        return;
    }

    if (!s_network_ready) {
        Serial.println(F("[SMS] no network"));
        return;
    }

    smsSend(eventType, uid, stateStr);
}

static bool parseJsonField(const char* json,
                           const char* field,
                           char* out,
                           byte outSize)
{
    char needle[18];
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
        char c = s_modem.read();
        if (c == '\r') continue;

        s_mqttBuf[s_mqttPos++] = c;
        s_mqttBuf[s_mqttPos] = '\0';
    }

    if (!strstr(s_mqttBuf, "+QMTRECV:")) return;
    if (!strchr(s_mqttBuf, '\n')) return;

    Serial.print(F("[MQTT] cmd "));
    Serial.println(s_mqttBuf);

    const char* jsonStart = strchr(s_mqttBuf, '{');
    if (!jsonStart) {
        s_mqttPos = 0;
        s_mqttBuf[0] = '\0';
        return;
    }

    char jsonBuf[88];
    strncpy(jsonBuf, jsonStart, sizeof(jsonBuf) - 1);
    jsonBuf[sizeof(jsonBuf) - 1] = '\0';

    s_mqttPos = 0;
    s_mqttBuf[0] = '\0';

    char* jsonEnd = strchr(jsonBuf, '}');
    if (jsonEnd) *(jsonEnd + 1) = '\0';

    char cmdStr[14];
    if (!parseJsonField(jsonBuf, "cmd", cmdStr, sizeof(cmdStr))) return;

    NbiotCommand command;
    memset(&command, 0, sizeof(command));
    command.type = CMD_NONE;

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
