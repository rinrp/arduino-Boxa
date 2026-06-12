#include "nbiot.h"
#include "config.h"
#include <SoftwareSerial.h>

// ============================================================
//  nbiot.cpp — Quectel BC92 + Orange Live Objects
//  BUILD: MQTT_RX_PARSE_UID_AND_PUBLISH_OK
//
//  Obiective:
//  - pastreaza conectarea MQTT care functiona deja;
//  - citeste modemul printr-o singura functie centrala;
//  - nu sterge +QMTRECV in timpul comenzilor AT;
//  - parseaza req + arg.uid din Live Objects;
//  - publica JSON valid pe dev/v1/data/rawjson/version1.
// ============================================================

#define MODEM_BAUD 9600

#define MQTT_RECONNECT_MAX_ATTEMPTS 5
#define MQTT_RECONNECT_COOLDOWN_MS  5000UL

#define AT_BUF_SIZE       120
#define MODEM_LINE_SIZE   220
#define CMD_JSON_SIZE     170
#define PAYLOAD_BUF_SIZE  130

static SoftwareSerial s_modem(MODEM_RX_PIN, MODEM_TX_PIN);

// ============================================================
//  Init state machine
// ============================================================

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
static bool s_networkReady = false;
static unsigned long s_lastHeartbeatMs = 0;
static NbiotCommandCallback s_cmdCallback = nullptr;

// ============================================================
//  Buffere RX separate
// ============================================================

static char s_atBuf[AT_BUF_SIZE];
static byte s_atPos = 0;

static char s_lineBuf[MODEM_LINE_SIZE];
static byte s_linePos = 0;
static bool s_promptSeen = false;

static char s_cmdJson[CMD_JSON_SIZE];
static bool s_cmdReady = false;

// ============================================================
//  Utilitare buffer
// ============================================================

static void resetAtBuffer() {
    s_atPos = 0;
    s_atBuf[0] = '\0';
    s_promptSeen = false;
}

static void appendAtLine(const char* line) {
    if (!line || !line[0]) return;

    byte len = strlen(line);

    if (len + s_atPos + 2 >= AT_BUF_SIZE) {
        // Daca nu mai incape, pastram ultima linie AT.
        s_atPos = 0;
        s_atBuf[0] = '\0';
    }

    while (*line && s_atPos < AT_BUF_SIZE - 2) {
        s_atBuf[s_atPos++] = *line++;
    }

    if (s_atPos < AT_BUF_SIZE - 2) {
        s_atBuf[s_atPos++] = '\n';
    }

    s_atBuf[s_atPos] = '\0';
}

static void finishLine() {
    s_lineBuf[s_linePos] = '\0';

    // Sterge \n final daca exista.
    while (s_linePos > 0 &&
           (s_lineBuf[s_linePos - 1] == '\n' || s_lineBuf[s_linePos - 1] == '\r')) {
        s_lineBuf[--s_linePos] = '\0';
    }

    if (strstr(s_lineBuf, "+QMTRECV:")) {
        const char* jsonStart = strchr(s_lineBuf, '{');
        const char* jsonEnd = strrchr(s_lineBuf, '}');

        if (jsonStart && jsonEnd && jsonEnd >= jsonStart) {
            byte n = (byte)(jsonEnd - jsonStart + 1);
            if (n >= CMD_JSON_SIZE) n = CMD_JSON_SIZE - 1;

            memcpy(s_cmdJson, jsonStart, n);
            s_cmdJson[n] = '\0';
            s_cmdReady = true;
        }
    } else {
        appendAtLine(s_lineBuf);
    }

    s_linePos = 0;
    s_lineBuf[0] = '\0';
}

// ============================================================
//  modemRead() — SINGURA functie care citeste din SoftwareSerial
// ============================================================

static void modemRead() {
    while (s_modem.available()) {
        char c = (char)s_modem.read();

        if (c == '\r') continue;

        if (c == '>') {
            s_promptSeen = true;
            continue;
        }

        if (s_linePos < MODEM_LINE_SIZE - 1) {
            s_lineBuf[s_linePos++] = c;
            s_lineBuf[s_linePos] = '\0';
        } else {
            // Linie prea mare: pastram inceputul pentru debug si o finalizam.
            finishLine();
        }

        if (c == '\n') {
            finishLine();
        }
    }
}

// ============================================================
//  AT helpers — fara flush din modem
// ============================================================

static void atWrite(const char* cmd) {
    resetAtBuffer();
    s_modem.println(cmd);
    Serial.print(F("[AT>>] "));
    Serial.println(cmd);
}

static void atWrite(const __FlashStringHelper* cmd) {
    resetAtBuffer();
    s_modem.println(cmd);
    Serial.print(F("[AT>>] "));
    Serial.println(cmd);
}

static bool atReadCheck(const char* expect) {
    modemRead();
    return strstr(s_atBuf, expect) != nullptr;
}

static bool atContains(const char* text) {
    modemRead();
    return strstr(s_atBuf, text) != nullptr;
}

static bool atHasError() {
    modemRead();
    return strstr(s_atBuf, "ERROR") != nullptr ||
           strstr(s_atBuf, "+CME ERROR") != nullptr ||
           strstr(s_atBuf, "+CMS ERROR") != nullptr;
}

static bool waitForText(const char* expect, unsigned long timeoutMs) {
    unsigned long start = millis();

    while (millis() - start < timeoutMs) {
        modemRead();

        if (strstr(s_atBuf, expect)) {
            return true;
        }

        if (strstr(s_atBuf, "ERROR") ||
            strstr(s_atBuf, "+CME ERROR") ||
            strstr(s_atBuf, "+CMS ERROR")) {
            return false;
        }
    }

    return false;
}

static bool waitForPrompt(unsigned long timeoutMs) {
    unsigned long start = millis();

    while (millis() - start < timeoutMs) {
        modemRead();
        if (s_promptSeen) {
            return true;
        }
    }

    return false;
}

static void printRawIfAny() {
    modemRead();
    if (s_atPos > 0) {
        Serial.print(F("[AT RAW] "));
        Serial.println(s_atBuf);
    }
}

static int parseCsqRssi() {
    modemRead();
    const char* p = strstr(s_atBuf, "+CSQ:");
    if (!p) return -1;

    p += 5;
    while (*p == ' ') p++;
    return atoi(p);
}

// ============================================================
//  MQTT connect helpers
// ============================================================

static void sendQmtOpen() {
    resetAtBuffer();

    s_modem.print(F("AT+QMTOPEN=0,\""));
    s_modem.print(LO_MQTT_HOST);
    s_modem.print(F("\","));
    s_modem.print(LO_MQTT_PORT);
    s_modem.println();

    Serial.print(F("[AT>>] QMTOPEN "));
    Serial.println(LO_MQTT_HOST);
}

static void sendQmtConn() {
    resetAtBuffer();

    s_modem.print(F("AT+QMTCONN=0,\""));
    s_modem.print(LO_MQTT_CLIENT);
    s_modem.print(F("\",\""));
    s_modem.print(LO_MQTT_USER);
    s_modem.print(F("\",\""));
    s_modem.print(LO_API_KEY);
    s_modem.println(F("\""));

    Serial.println(F("[AT>>] QMTCONN ***"));
}

static void sendQmtSub() {
    resetAtBuffer();

    s_modem.print(F("AT+QMTSUB=0,1,\""));
    s_modem.print(LO_TOPIC_SUB);
    s_modem.println(F("\",1"));

    Serial.print(F("[AT>>] QMTSUB "));
    Serial.println(LO_TOPIC_SUB);
}

static void sendQmtDisc() {
    resetAtBuffer();
    s_modem.println(F("AT+QMTDISC=0"));
    Serial.println(F("[AT>>] QMTDISC"));
}

static void sendQmtClose() {
    resetAtBuffer();
    s_modem.println(F("AT+QMTCLOSE=0"));
    Serial.println(F("[AT>>] QMTCLOSE"));
}

static void beginReconnect(const __FlashStringHelper* reason) {
    Serial.print(F("[NB] Reconnect: "));
    Serial.println(reason);

    s_connected = false;

    if (s_mqttReconnectAttempts >= MQTT_RECONNECT_MAX_ATTEMPTS) {
        Serial.println(F("[NB] reconnect esuat dupa 5 incercari"));
        s_initState = INIT_FAILED;
        return;
    }

    s_mqttReconnectAttempts++;
    s_initState = INIT_MQTT_DISC;
    s_initStepMs = millis();
    s_initStepTimeout = 3000;
    sendQmtDisc();
}

// ============================================================
//  Init
// ============================================================

bool nbiot_init() {
    s_modem.begin(MODEM_BAUD);
    delay(500);

    resetAtBuffer();
    s_linePos = 0;
    s_lineBuf[0] = '\0';
    s_cmdJson[0] = '\0';
    s_cmdReady = false;

    s_connected = false;
    s_networkReady = false;
    s_atRetries = 0;
    s_stepRetries = 0;
    s_ceregRetries = 0;
    s_mqttReconnectAttempts = 0;
    s_lastHeartbeatMs = 0;

    Serial.println(F("[BUILD] MQTT_RX_PARSE_UID_AND_PUBLISH_OK"));
    Serial.println(F("[NB-IoT] Init non-blocker..."));

    s_initState = INIT_AT_TEST;
    s_initStepMs = millis();
    s_initStepTimeout = 5000;

    atWrite("AT");
    return true;
}

void nbiot_initTick() {
    modemRead();

    if (s_initState == INIT_IDLE || s_initState == INIT_DONE || s_initState == INIT_FAILED) {
        return;
    }

    unsigned long now = millis();

    switch (s_initState) {
        case INIT_AT_TEST:
            if (atReadCheck("OK")) {
                Serial.println(F("[NB-IoT] Modem OK"));
                s_initState = INIT_ECHO_OFF;
                s_initStepMs = now;
                s_initStepTimeout = 3000;
                s_stepRetries = 0;
                atWrite(F("ATE0"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_atRetries++;
                if (s_atRetries >= 5) {
                    Serial.println(F("[NB-IoT] modem nu raspunde"));
                    s_initState = INIT_FAILED;
                } else {
                    s_initStepMs = now;
                    atWrite("AT");
                }
            }
            break;

        case INIT_ECHO_OFF:
            if (atReadCheck("OK")) {
                Serial.println(F("[NB-IoT] Echo off"));
                s_initState = INIT_APN;
                s_initStepMs = now;
                s_initStepTimeout = 10000;
                s_stepRetries = 0;
                atWrite(F("AT+QCGDEFCONT=\"IP\",\"net\""));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB-IoT] ATE0 fail"));
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
                Serial.println(F("[NB-IoT] APN OK"));
                s_initState = INIT_CEREG;
                s_initStepMs = now;
                s_initStepTimeout = 5000;
                s_ceregRetries = 0;
                atWrite(F("AT+CEREG?"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB-IoT] APN fail"));
                    printRawIfAny();
                    s_initState = INIT_FAILED;
                } else {
                    s_initStepMs = now;
                    atWrite(F("AT+QCGDEFCONT=\"IP\",\"net\""));
                }
            }
            break;

        case INIT_CEREG:
            if (atContains("+CEREG: 0,1") || atContains("+CEREG: 0,5") ||
                atContains("+CEREG: 1,1") || atContains("+CEREG: 1,5")) {
                Serial.println(F("[NB-IoT] Retea OK"));
                s_networkReady = true;
                s_initState = INIT_CSQ;
                s_initStepMs = now;
                s_initStepTimeout = 5000;
                s_stepRetries = 0;
                atWrite(F("AT+CSQ"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_ceregRetries++;
                if (s_ceregRetries >= 12) {
                    Serial.println(F("[NB-IoT] fara retea"));
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
                Serial.print(F("[NB-IoT] CSQ="));
                Serial.println(parseCsqRssi());
                s_initState = INIT_MQTT_VERSION;
                s_initStepMs = now;
                s_initStepTimeout = 5000;
                s_stepRetries = 0;
                atWrite(F("AT+QMTCFG=\"version\",0,1"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 5) {
                    Serial.println(F("[NB-IoT] CSQ timeout"));
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
                Serial.println(F("[NB-IoT] MQTT v3.1.1 OK"));
                s_initState = INIT_MQTT_SSL;
                s_initStepMs = now;
                s_initStepTimeout = 5000;
                s_stepRetries = 0;
                atWrite(F("AT+QMTCFG=\"ssl\",0,1,1,0"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB-IoT] MQTT version fail"));
                    printRawIfAny();
                    beginReconnect(F("version"));
                } else {
                    s_initStepMs = now;
                    atWrite(F("AT+QMTCFG=\"version\",0,1"));
                }
            }
            break;

        case INIT_MQTT_SSL:
            if (atReadCheck("OK")) {
                Serial.println(F("[NB-IoT] SSL OK"));
                s_initState = INIT_MQTOPEN;
                s_initStepMs = now;
                s_initStepTimeout = 30000;
                s_stepRetries = 0;
                sendQmtOpen();
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB-IoT] SSL fail"));
                    printRawIfAny();
                    beginReconnect(F("ssl"));
                } else {
                    s_initStepMs = now;
                    atWrite(F("AT+QMTCFG=\"ssl\",0,1,1,0"));
                }
            }
            break;

        case INIT_MQTOPEN:
            if (atContains("+QMTOPEN: 0,0")) {
                Serial.println(F("[NB-IoT] Socket deschis!"));
                s_initState = INIT_MQTCONN;
                s_initStepMs = now;
                s_initStepTimeout = 30000;
                s_stepRetries = 0;
                sendQmtConn();
            } else if (atHasError() || atContains("+QMTOPEN: 0,")) {
                if (!strstr(s_atBuf, "+QMTOPEN: 0,0")) {
                    Serial.println(F("[NB-IoT] QMTOPEN fail"));
                    printRawIfAny();
                    beginReconnect(F("QMTOPEN"));
                }
            } else if (now - s_initStepMs > s_initStepTimeout) {
                Serial.println(F("[NB-IoT] QMTOPEN timeout"));
                printRawIfAny();
                beginReconnect(F("QMTOPEN timeout"));
            }
            break;

        case INIT_MQTCONN:
            if (atContains("+QMTCONN: 0,0,0")) {
                Serial.println(F("[NB-IoT] Autentificat!"));
                s_initState = INIT_MQTSUB;
                s_initStepMs = now;
                s_initStepTimeout = 10000;
                s_stepRetries = 0;
                sendQmtSub();
            } else if (atContains("+QMTCONN: 0,0,4") || atContains("+QMTCONN: 0,0,5")) {
                Serial.println(F("[NB-IoT] credentiale Live Objects invalide"));
                beginReconnect(F("auth"));
            } else if (atHasError()) {
                Serial.println(F("[NB-IoT] QMTCONN error"));
                printRawIfAny();
                beginReconnect(F("QMTCONN"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB-IoT] QMTCONN timeout"));
                    printRawIfAny();
                    beginReconnect(F("QMTCONN timeout"));
                } else {
                    s_initStepMs = now;
                    sendQmtConn();
                }
            }
            break;

        case INIT_MQTSUB:
            if (atContains("+QMTSUB:")) {
                s_connected = true;
                s_initState = INIT_DONE;
                s_mqttReconnectAttempts = 0;
                Serial.println(F("[NB-IoT] Subscris dev/cmd"));
                Serial.println(F("[NB-IoT] === ONLINE ==="));
                nbiot_publish(EVT_SYSTEM_STATUS, "", "BOOT");
            } else if (atHasError()) {
                Serial.println(F("[NB-IoT] QMTSUB error"));
                printRawIfAny();
                beginReconnect(F("QMTSUB"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) {
                    Serial.println(F("[NB-IoT] QMTSUB timeout"));
                    printRawIfAny();
                    beginReconnect(F("QMTSUB timeout"));
                } else {
                    s_initStepMs = now;
                    sendQmtSub();
                }
            }
            break;

        case INIT_MQTT_DISC:
            if (atContains("+QMTDISC:") || atContains("OK") || atHasError() ||
                now - s_initStepMs > s_initStepTimeout) {
                s_initState = INIT_MQTT_CLOSE;
                s_initStepMs = now;
                s_initStepTimeout = 5000;
                sendQmtClose();
            }
            break;

        case INIT_MQTT_CLOSE:
            if (atContains("+QMTCLOSE:") || atContains("OK") || atHasError() ||
                now - s_initStepMs > s_initStepTimeout) {
                Serial.println(F("[NB-IoT] reconnect cooldown"));
                s_initState = INIT_RECONNECT_COOLDOWN;
                s_initStepMs = now;
                s_initStepTimeout = MQTT_RECONNECT_COOLDOWN_MS;
            }
            break;

        case INIT_RECONNECT_COOLDOWN:
            if (now - s_initStepMs >= s_initStepTimeout) {
                Serial.println(F("[NB-IoT] reconnect start"));
                s_connected = false;
                s_networkReady = false;
                s_atRetries = 0;
                s_stepRetries = 0;
                s_ceregRetries = 0;
                resetAtBuffer();
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
//  Publish MQTT
// ============================================================

static bool isCriticalEvent(const char* eventType) {
    return strcmp(eventType, EVT_ALARM_START) == 0 ||
           strcmp(eventType, EVT_ACCESS_DENIED) == 0 ||
           strcmp(eventType, EVT_REMOTE_OPEN) == 0;
}

static void smsFallback(const char* eventType, const char* uid, const char* stateStr) {
#if ENABLE_SMS_FALLBACK
    if (!s_networkReady || !isCriticalEvent(eventType)) return;

    Serial.print(F("[SMS] fallback "));
    Serial.println(eventType);

    // Simplu si limitat: SMS-ul ramane optional pentru a nu bloca testarea MQTT.
    resetAtBuffer();
    s_modem.println(F("AT+CMGF=1"));
    waitForText("OK", 1500);

    char cmd[28];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", LO_SMS_ADMIN_NR);
    resetAtBuffer();
    s_modem.println(cmd);
    if (waitForPrompt(4000)) {
        s_modem.print(F("EVT:"));
        s_modem.print(eventType);
        s_modem.print(F(",ST:"));
        s_modem.print(stateStr ? stateStr : "");
        if (uid && uid[0]) {
            s_modem.print(F(",UID:"));
            s_modem.print(uid);
        }
        s_modem.write(26);
        waitForText("OK", 8000);
    }
#else
    (void)eventType;
    (void)uid;
    (void)stateStr;
#endif
}

void nbiot_publish(const char* eventType,
                   const char* uid,
                   const char* stateStr) {
    modemRead();

    if (s_initState != INIT_DONE) {
        Serial.print(F("[MQTT] Init in curs, ignorat: "));
        Serial.println(eventType ? eventType : "");
        return;
    }

    if (!s_connected) {
        smsFallback(eventType, uid, stateStr);
        return;
    }

    char payload[PAYLOAD_BUF_SIZE];
    int written = snprintf(payload, sizeof(payload),
                           "{\"s\":\"%s\",\"v\":{\"event\":\"%s\",\"uid\":\"%s\",\"state\":\"%s\"}}",
                           LO_STREAM_ID,
                           eventType ? eventType : "",
                           uid ? uid : "",
                           stateStr ? stateStr : "");

    if (written <= 0 || written >= (int)sizeof(payload)) {
        Serial.println(F("[MQTT PUB] payload prea mare"));
        return;
    }

    Serial.print(F("[MQTT PUB] payload="));
    Serial.println(payload);

    resetAtBuffer();

    s_modem.print(F("AT+QMTPUB=0,1,0,0,\""));
    s_modem.print(LO_TOPIC_PUB);
    s_modem.print(F("\","));
    s_modem.println(strlen(payload));

    if (!waitForPrompt(4000)) {
        Serial.println(F("[MQTT PUB] no prompt"));
        smsFallback(eventType, uid, stateStr);
        return;
    }

    resetAtBuffer();
    s_modem.print(payload);

    if (waitForText("+QMTPUB:", 7000)) {
        Serial.println(F("[MQTT PUB] OK"));
    } else {
        Serial.println(F("[MQTT PUB] timeout"));
        // Nu reconectam agresiv pentru evenimente minore.
        smsFallback(eventType, uid, stateStr);
    }
}

// ============================================================
//  JSON parser fara ArduinoJson si fara String
// ============================================================

static const char* skipSpaces(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static bool jsonStringField(const char* json, const char* key, char* out, byte outSize) {
    if (!json || !key || !out || outSize == 0) return false;

    char needle[18];
    snprintf(needle, sizeof(needle), "\"%s\"", key);

    const char* p = strstr(json, needle);
    if (!p) return false;

    p += strlen(needle);
    p = skipSpaces(p);
    if (*p != ':') return false;
    p++;
    p = skipSpaces(p);
    if (*p != '"') return false;
    p++;

    byte i = 0;
    while (*p && *p != '"' && i < outSize - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';

    return i > 0 && *p == '"';
}

static bool jsonCidField(const char* json, char* out, byte outSize) {
    if (!json || !out || outSize == 0) return false;

    const char* p = strstr(json, "\"cid\"");
    if (!p) return false;

    p += 5;
    p = skipSpaces(p);
    if (*p != ':') return false;
    p++;
    p = skipSpaces(p);

    byte i = 0;

    if (*p == '"') {
        p++;
        while (*p && *p != '"' && i < outSize - 1) {
            out[i++] = *p++;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && i < outSize - 1) {
            if (*p != ' ') out[i++] = *p;
            p++;
        }
    }

    out[i] = '\0';
    return i > 0;
}

// ============================================================
//  Comenzi MQTT primite
// ============================================================

void nbiot_checkCommands() {
    modemRead();

    if (!s_cmdReady || !s_cmdCallback) {
        return;
    }

    s_cmdReady = false;

    Serial.print(F("[MQTT RX] raw="));
    Serial.println(s_cmdJson);

    NbiotCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_NONE;

    char req[16];
    if (!jsonStringField(s_cmdJson, "req", req, sizeof(req))) {
        if (!jsonStringField(s_cmdJson, "cmd", req, sizeof(req))) {
            Serial.println(F("[CMD] lipseste req/cmd"));
            return;
        }
    }

    jsonCidField(s_cmdJson, cmd.cid, sizeof(cmd.cid));

    Serial.print(F("[CMD] req="));
    Serial.println(req);

    if (strcmp(req, "open") == 0) {
        cmd.type = CMD_OPEN;
        Serial.println(F("[CMD] open"));
    } else if (strcmp(req, "status") == 0) {
        cmd.type = CMD_STATUS;
        Serial.println(F("[CMD] status"));
    } else if (strcmp(req, "card_add") == 0) {
        cmd.type = CMD_CARD_ADD;
        if (!jsonStringField(s_cmdJson, "uid", cmd.uid, sizeof(cmd.uid))) {
            Serial.println(F("[CMD] card_add: uid lipsa"));
            return;
        }
        Serial.print(F("[CMD] uid="));
        Serial.println(cmd.uid);
    } else if (strcmp(req, "card_remove") == 0) {
        cmd.type = CMD_CARD_REMOVE;
        if (!jsonStringField(s_cmdJson, "uid", cmd.uid, sizeof(cmd.uid))) {
            Serial.println(F("[CMD] card_remove: uid lipsa"));
            return;
        }
        Serial.print(F("[CMD] uid="));
        Serial.println(cmd.uid);
    } else {
        Serial.print(F("[CMD] necunoscuta: "));
        Serial.println(req);
        return;
    }

    s_cmdJson[0] = '\0';
    s_cmdCallback(cmd);
}

void nbiot_setCommandCallback(NbiotCommandCallback cb) {
    s_cmdCallback = cb;
}

void nbiot_heartbeatTick(const char* stateStr) {
    if (!s_connected || s_initState != INIT_DONE) return;

    unsigned long now = millis();
    if (now - s_lastHeartbeatMs >= NBIOT_HEARTBEAT_MS) {
        s_lastHeartbeatMs = now;
        nbiot_publish(EVT_SYSTEM_STATUS, "", stateStr);
    }
}

bool nbiot_isConnected() {
    return s_connected;
}
