#include "nbiot.h"
#include "config.h"
#include <SoftwareSerial.h>

// ============================================================
//  nbiot.cpp — BC92 + Orange Live Objects
//  BUILD: MQTT_RX_NONBLOCKING_NO_LOSS
//
//  Probleme rezolvate față de versiunea anterioară:
//
//  1. atWrite() nu mai face flush pe SoftwareSerial.
//     Anterior: while(s_modem.available()) s_modem.read();
//     ștergea +QMTRECV sosit între comenzi AT.
//     Acum: datele din modem sunt citite ÎNTOTDEAUNA prin
//     modemRead() care le rutează: +QMTRECV → cmdBuf,
//     restul → atBuf.
//
//  2. atSendBlocking() nu mai pierde +QMTRECV.
//     Anterior: citea într-un buf local și arunca tot ce nu
//     era răspunsul așteptat. Acum rulează prin modemRead().
//
//  3. Parser JSON dual-format:
//     - Live Objects: {"req":"card_add","arg":{"uid":"..."}}
//     - Legacy:       {"cmd":"card_add","uid":"..."}
//
//  4. Buffer de comenzi separat (CMD_BUF_SIZE bytes) care
//     acumulează date fără să interfereze cu bufferul AT.
//
//  5. Nu folosim AT+QMTCFG="recv/mode" — BC92 răspunde ERROR.
//     Modemul trimite +QMTRECV automat (URC mode implicit).
// ============================================================

#define MODEM_BAUD 9600
static SoftwareSerial s_modem(MODEM_RX_PIN, MODEM_TX_PIN);

// ============================================================
//  Buffere separate: AT vs comenzi MQTT
//
//  Dimensiuni calculate pentru 2KB RAM Arduino Uno:
//  - AT buf: 80 bytes  — răspunsuri AT standard (OK, +QMTPUB etc)
//  - CMD buf: 110 bytes — +QMTRECV: 0,0,dev/cmd,{"req":...}
//    Un mesaj Live Objects tipic: ~80-100 chars
// ============================================================
#define AT_BUF_SIZE  80
#define CMD_BUF_SIZE 110

static char  s_atBuf[AT_BUF_SIZE];
static byte  s_atPos = 0;

static char  s_cmdBuf[CMD_BUF_SIZE];
static byte  s_cmdPos = 0;
static bool  s_cmdReady = false;   // true cand avem o linie completa

// ============================================================
//  State machine initializare
// ============================================================
enum InitState : byte {
    INIT_IDLE,
    INIT_AT_TEST,
    INIT_ECHO_OFF,
    INIT_APN,
    INIT_CEREG,
    INIT_MQTVER,     // AT+QMTCFG="version",0,1
    INIT_MQTSSL,     // AT+QMTCFG="ssl",0,1,1,0
    INIT_MQTOPEN,    // AT+QMTOPEN
    INIT_MQTCONN,    // AT+QMTCONN
    INIT_MQTSUB,     // AT+QMTSUB
    INIT_DONE,
    INIT_FAILED
};

static InitState     s_initState       = INIT_IDLE;
static unsigned long s_initStepMs      = 0;
static unsigned long s_initStepTimeout = 0;
static byte          s_ceregRetries    = 0;
static byte          s_atRetries       = 0;
static byte          s_stepRetries     = 0;

static bool                  s_connected   = false;
static bool                  s_netReady    = false;
static NbiotCommandCallback  s_cmdCb       = nullptr;
static unsigned long         s_lastHbMs    = 0;

// ============================================================
//  modemRead() — SINGURA funcție care citește din SoftwareSerial
//
//  Rutează fiecare byte:
//   - dacă linia curentă din cmdBuf conține "+QMTRECV" sau
//     dacă cmdBuf e deja în curs de acumulare → cmdBuf
//   - altfel → atBuf
//
//  Apelat din: atReadCheck(), atSendBlocking(), nbiot_checkCommands()
// ============================================================
static void modemRead() {
    while (s_modem.available()) {
        char c = (char)s_modem.read();

        // Decide rutarea: dacă cmdBuf are deja ceva sau
        // dacă atBuf conține "+QMTRECV" la început
        bool routeToCmd = (s_cmdPos > 0);

        // Dacă cmdBuf e gol, verifica daca atBuf contine prefix QMTRECV
        if (!routeToCmd && s_atPos >= 8) {
            // Cauta "+QMTRECV" in atBuf
            routeToCmd = (strstr(s_atBuf, "+QMTRECV") != nullptr);
            if (routeToCmd) {
                // Muta ce avem din atBuf in cmdBuf
                byte move = s_atPos < CMD_BUF_SIZE - 1 ? s_atPos : CMD_BUF_SIZE - 2;
                memcpy(s_cmdBuf, s_atBuf, move);
                s_cmdPos = move;
                s_cmdBuf[s_cmdPos] = '\0';
                s_atPos = 0;
                s_atBuf[0] = '\0';
            }
        }

        if (routeToCmd) {
            if (s_cmdPos < CMD_BUF_SIZE - 1) {
                s_cmdBuf[s_cmdPos++] = c;
                s_cmdBuf[s_cmdPos]   = '\0';
            }
            // Linie completa?
            if (c == '\n') {
                s_cmdReady = true;
            }
        } else {
            // Verifica daca noul caracter ar face bufferul sa contina QMTRECV
            if (s_atPos < AT_BUF_SIZE - 1) {
                s_atBuf[s_atPos++] = c;
                s_atBuf[s_atPos]   = '\0';

                // Re-verifica dupa adaugare
                if (strstr(s_atBuf, "+QMTRECV") != nullptr) {
                    // Muta in cmdBuf
                    byte move = s_atPos < CMD_BUF_SIZE - 1 ? s_atPos : CMD_BUF_SIZE - 2;
                    memcpy(s_cmdBuf, s_atBuf, move);
                    s_cmdPos = move;
                    s_cmdBuf[s_cmdPos] = '\0';
                    s_atPos = 0;
                    s_atBuf[0] = '\0';
                }
            }
            if (c == '\n' && s_atPos > 0) {
                // Linie AT completa — reset pozitie pentru urmatoarea linie
                // dar pastreaza continutul pentru atReadCheck
            }
        }
    }
}

// ============================================================
//  atWrite() — trimite comanda AT fara sa stearga bufferele
//  NU mai face flush pe SoftwareSerial!
// ============================================================
static void atWrite(const __FlashStringHelper* cmd) {
    // Reset doar atBuf — nu cmdBuf!
    s_atPos = 0;
    s_atBuf[0] = '\0';

    s_modem.println(cmd);
    Serial.print(F("[AT>>] "));
    Serial.println(cmd);
}

static void atWrite(const char* cmd) {
    s_atPos = 0;
    s_atBuf[0] = '\0';

    s_modem.println(cmd);
    Serial.print(F("[AT>>] "));
    Serial.println(cmd);
}

// ============================================================
//  atReadCheck() — cauta subsirul in atBuf
//  Apeleaza modemRead() pentru a umple bufferul.
// ============================================================
static bool atReadCheck(const char* expect) {
    modemRead();
    return (strstr(s_atBuf, expect) != nullptr);
}

// ============================================================
//  atSendBlocking() — blocker scurt DOAR pentru publish
//  Apeleaza modemRead() — nu pierde +QMTRECV
// ============================================================
static bool atSendBlocking(const char* expect, unsigned int timeoutMs) {
    s_atPos = 0;
    s_atBuf[0] = '\0';

    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        modemRead();
        if (strstr(s_atBuf, expect)) {
            Serial.print(F("[AT<<] "));
            Serial.println(s_atBuf);
            return true;
        }
    }
    Serial.println(F("[AT] timeout"));
    return false;
}

// ============================================================
//  nbiot_init()
// ============================================================
bool nbiot_init() {
    s_modem.begin(MODEM_BAUD);
    delay(500);

    Serial.println(F("[BUILD] MQTT_RX_NONBLOCKING_NO_LOSS"));
    Serial.println(F("[NB-IoT] Init non-blocker..."));
    Serial.println(F("[NB-IoT] RFID/senzori activi imediat."));

    s_initState       = INIT_AT_TEST;
    s_initStepMs      = millis();
    s_initStepTimeout = 5000;
    s_atRetries       = 0;
    s_ceregRetries    = 0;
    s_stepRetries     = 0;

    atWrite("AT");
    return true;
}

// ============================================================
//  nbiot_initTick() — state machine initializare
//
//  Secventa AT commands (pastrata identica cu ce functioneaza):
//  1. AT
//  2. ATE0
//  3. AT+QCGDEFCONT="IP","net"
//  4. AT+CEREG?
//  5. AT+CSQ  (optional, doar log)
//  6. AT+QMTCFG="version",0,1
//  7. AT+QMTCFG="ssl",0,1,1,0
//  8. AT+QMTOPEN=0,"liveobjects...",8883
//  9. AT+QMTCONN=0,"RFID","json+device","<apikey>"
// 10. AT+QMTSUB=0,1,"dev/cmd",1
// ============================================================
void nbiot_initTick() {
    if (s_initState == INIT_DONE || s_initState == INIT_FAILED) return;
    if (s_initState == INIT_IDLE) return;

    modemRead();  // citeste constant fara sa piarda date

    unsigned long now = millis();

    switch (s_initState) {

        case INIT_AT_TEST:
            if (strstr(s_atBuf, "OK")) {
                Serial.println(F("[NB-IoT] Modem OK"));
                s_initState = INIT_ECHO_OFF;
                s_initStepMs = now;
                s_stepRetries = 0;
                atWrite("ATE0");
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_atRetries++;
                if (s_atRetries >= 5) {
                    Serial.println(F("[NB-IoT] EROARE: modem nu raspunde"));
                    Serial.println(F("[NB-IoT] Verifica: D2->BC92_RX, D3->BC92_TX"));
                    Serial.println(F("[NB-IoT] Sistem LOCAL activ."));
                    s_initState = INIT_FAILED;
                } else {
                    Serial.print(F("[NB-IoT] Retry AT "));
                    Serial.println(s_atRetries);
                    s_initStepMs = now;
                    atWrite("AT");
                }
            }
            break;

        case INIT_ECHO_OFF:
            if (strstr(s_atBuf, "OK")) {
                Serial.println(F("[NB-IoT] Echo off"));
                s_initState = INIT_APN;
                s_initStepMs = now;
                s_initStepTimeout = 10000;
                s_stepRetries = 0;
                atWrite(F("AT+QCGDEFCONT=\"IP\",\"net\""));
            } else if (now - s_initStepMs > 3000) {
                s_stepRetries++;
                if (s_stepRetries >= 3) { s_initState = INIT_FAILED; return; }
                s_initStepMs = now;
                atWrite("ATE0");
            }
            break;

        case INIT_APN:
            if (strstr(s_atBuf, "OK")) {
                s_netReady = true;
                Serial.println(F("[NB-IoT] APN OK"));
                s_initState = INIT_CEREG;
                s_initStepMs = now;
                s_initStepTimeout = 5000;
                s_ceregRetries = 0;
                atWrite(F("AT+CEREG?"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) { s_initState = INIT_FAILED; return; }
                Serial.print(F("[NB-IoT] Retry APN "));
                Serial.println(s_stepRetries);
                s_initStepMs = now;
                atWrite(F("AT+QCGDEFCONT=\"IP\",\"net\""));
            }
            break;

        case INIT_CEREG:
            if (strstr(s_atBuf, "+CEREG: 0,1") || strstr(s_atBuf, "+CEREG: 0,5")) {
                Serial.println(F("[NB-IoT] Retea OK"));
                // Log semnal
                s_modem.println(F("AT+CSQ"));
                delay(200);
                modemRead();
                Serial.print(F("[NB-IoT] CSQ: "));
                Serial.println(s_atBuf);

                s_initState = INIT_MQTVER;
                s_initStepMs = now;
                s_initStepTimeout = 5000;
                s_stepRetries = 0;
                atWrite(F("AT+QMTCFG=\"version\",0,1"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_ceregRetries++;
                if (s_ceregRetries >= 12) {
                    Serial.println(F("[NB-IoT] EROARE: fara retea"));
                    s_initState = INIT_FAILED;
                } else {
                    s_initStepMs = now;
                    atWrite(F("AT+CEREG?"));
                }
            }
            break;

        case INIT_MQTVER:
            if (strstr(s_atBuf, "OK")) {
                Serial.println(F("[NB-IoT] MQTT v3.1.1 OK"));
                s_initState = INIT_MQTSSL;
                s_initStepMs = now;
                s_stepRetries = 0;
                atWrite(F("AT+QMTCFG=\"ssl\",0,1,1,0"));
            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) { s_initState = INIT_FAILED; return; }
                s_initStepMs = now;
                atWrite(F("AT+QMTCFG=\"version\",0,1"));
            }
            break;

        case INIT_MQTSSL:
            if (strstr(s_atBuf, "OK")) {
                Serial.println(F("[NB-IoT] SSL OK"));
                s_initState = INIT_MQTOPEN;
                s_initStepMs = now;
                s_initStepTimeout = 20000;
                s_stepRetries = 0;

                // AT+QMTOPEN fragmentat — economie RAM
                Serial.print(F("[AT>>] AT+QMTOPEN=0,\""));
                Serial.print(F(LO_MQTT_HOST));
                Serial.print(F("\","));
                Serial.println(LO_MQTT_PORT);

                s_modem.print(F("AT+QMTOPEN=0,\""));
                s_modem.print(F(LO_MQTT_HOST));
                s_modem.print(F("\","));
                s_modem.print(LO_MQTT_PORT);
                s_modem.println();

                s_atPos = 0; s_atBuf[0] = '\0';
            } else if (now - s_initStepMs > 5000) {
                s_stepRetries++;
                if (s_stepRetries >= 3) { s_initState = INIT_FAILED; return; }
                s_initStepMs = now;
                atWrite(F("AT+QMTCFG=\"ssl\",0,1,1,0"));
            }
            break;

        case INIT_MQTOPEN:
            if (strstr(s_atBuf, "+QMTOPEN: 0,0")) {
                Serial.println(F("[NB-IoT] Socket deschis!"));
                s_initState = INIT_MQTCONN;
                s_initStepMs = now;
                s_initStepTimeout = 10000;
                s_stepRetries = 0;

                Serial.println(F("[AT>>] AT+QMTCONN=0,RFID,json+device,***"));
                s_modem.print(F("AT+QMTCONN=0,\""));
                s_modem.print(F(LO_MQTT_CLIENT));
                s_modem.print(F("\",\""));
                s_modem.print(F(LO_MQTT_USER));
                s_modem.print(F("\",\""));
                s_modem.print(F(LO_API_KEY));
                s_modem.println(F("\""));

                s_atPos = 0; s_atBuf[0] = '\0';

            } else if (strstr(s_atBuf, "+QMTOPEN: 0,-1")) {
                Serial.println(F("[NB-IoT] EROARE socket — TLS?"));
                s_initState = INIT_FAILED;

            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) { s_initState = INIT_FAILED; return; }
                Serial.print(F("[NB-IoT] Retry QMTOPEN "));
                Serial.println(s_stepRetries);
                s_initStepMs = now;
                s_atPos = 0; s_atBuf[0] = '\0';

                s_modem.print(F("AT+QMTOPEN=0,\""));
                s_modem.print(F(LO_MQTT_HOST));
                s_modem.print(F("\","));
                s_modem.print(LO_MQTT_PORT);
                s_modem.println();
            }
            break;

        case INIT_MQTCONN:
            if (strstr(s_atBuf, "+QMTCONN: 0,0,0")) {
                Serial.println(F("[NB-IoT] Autentificat!"));
                s_initState = INIT_MQTSUB;
                s_initStepMs = now;
                s_initStepTimeout = 5000;
                s_stepRetries = 0;

                Serial.print(F("[AT>>] AT+QMTSUB=0,1,\""));
                Serial.print(F(LO_TOPIC_SUB));
                Serial.println(F("\",1"));

                s_modem.print(F("AT+QMTSUB=0,1,\""));
                s_modem.print(F(LO_TOPIC_SUB));
                s_modem.println(F("\",1"));

                s_atPos = 0; s_atBuf[0] = '\0';

            } else if (strstr(s_atBuf, "+QMTCONN: 0,0,4") ||
                       strstr(s_atBuf, "+QMTCONN: 0,0,5")) {
                Serial.println(F("[NB-IoT] EROARE: credentiale invalide!"));
                Serial.println(F("[NB-IoT] Verifica LO_API_KEY in nbiot.h"));
                s_initState = INIT_FAILED;

            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) { s_initState = INIT_FAILED; return; }
                Serial.print(F("[NB-IoT] Retry QMTCONN "));
                Serial.println(s_stepRetries);
                s_initStepMs = now;
                s_atPos = 0; s_atBuf[0] = '\0';

                s_modem.print(F("AT+QMTCONN=0,\""));
                s_modem.print(F(LO_MQTT_CLIENT));
                s_modem.print(F("\",\""));
                s_modem.print(F(LO_MQTT_USER));
                s_modem.print(F("\",\""));
                s_modem.print(F(LO_API_KEY));
                s_modem.println(F("\""));
            }
            break;

        case INIT_MQTSUB:
            if (strstr(s_atBuf, "+QMTSUB:")) {
                s_connected = true;
                s_initState = INIT_DONE;
                Serial.println(F("[NB-IoT] Subscris dev/cmd"));
                Serial.println(F("[NB-IoT] === ONLINE ==="));
                nbiot_publish(EVT_SYSTEM_STATUS, "", "BOOT");

            } else if (now - s_initStepMs > s_initStepTimeout) {
                s_stepRetries++;
                if (s_stepRetries >= 3) { s_initState = INIT_FAILED; return; }
                s_initStepMs = now;
                s_atPos = 0; s_atBuf[0] = '\0';

                s_modem.print(F("AT+QMTSUB=0,1,\""));
                s_modem.print(F(LO_TOPIC_SUB));
                s_modem.println(F("\",1"));
            }
            break;

        default:
            break;
    }
}

// ============================================================
//  nbiot_publish() — MQTT publish fragmentat
//
//  Nu foloseste snprintf pentru payload — economie 200+ bytes.
//  atSendBlocking() apeleaza modemRead() — nu pierde +QMTRECV.
// ============================================================
void nbiot_publish(const char* eventType,
                   const char* uid,
                   const char* stateStr) {

    if (s_initState != INIT_DONE && s_initState != INIT_FAILED) {
        Serial.print(F("[MQTT] Init in curs, ignorat: "));
        Serial.println(eventType);
        return;
    }

    if (!s_connected) {
#if ENABLE_SMS_FALLBACK
        bool isCritic = (strcmp(eventType, EVT_ALARM_START)  == 0 ||
                         strcmp(eventType, EVT_ACCESS_DENIED) == 0);
        if (isCritic && s_netReady) {
            // SMS fallback
            s_modem.println(F("AT+CMGF=1"));
            delay(200); modemRead();
            char cmd[30];
            snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", LO_SMS_ADMIN_NR);
            s_modem.println(cmd);
            delay(300); modemRead();
            s_modem.print(F("EVT:"));
            s_modem.print(eventType);
            s_modem.print(F(" ST:"));
            s_modem.print(stateStr);
            if (uid && uid[0]) { s_modem.print(F(" UID:")); s_modem.print(uid); }
            s_modem.write(0x1A);
            Serial.println(F("[SMS] trimis"));
        }
#else
        Serial.print(F("[MQTT] Offline: "));
        Serial.println(eventType);
#endif
        return;
    }

    // Publish fragmentat — fara buffer intermediar
    s_modem.print(F("AT+QMTPUB=0,1,0,0,\""));
    s_modem.print(F(LO_TOPIC_PUB));
    s_modem.print(F("\",\""));
    // Payload JSON simplu — fara escape dublu (modemul BC92 accepta JSON direct)
    s_modem.print(F("{\"s\":\""));
    s_modem.print(F(LO_STREAM_ID));
    s_modem.print(F("\",\"v\":{\"event\":\""));
    s_modem.print(eventType);
    s_modem.print(F("\",\"uid\":\""));
    s_modem.print(uid ? uid : "");
    s_modem.print(F("\",\"state\":\""));
    s_modem.print(stateStr ? stateStr : "");
    s_modem.println(F("\"}}\""));

    Serial.print(F("[MQTT] Pub: "));
    Serial.println(eventType);

    // Asteapta +QMTPUB max 3s — modemRead() e apelat intern
    atSendBlocking("+QMTPUB:", 3000);
}

// ============================================================
//  Parser JSON — suporta ambele formate:
//
//  Format Live Objects:
//    {"req":"card_add","arg":{"uid":"CA:FD:A1:80"},"cid":123}
//
//  Format legacy (compatibilitate):
//    {"cmd":"card_add","uid":"CA:FD:A1:80"}
//
//  Parsare minimala fara librarie — economie RAM.
//  Cauta "cheie":"valoare" in sir.
// ============================================================

// Cauta valoarea unui camp string: "key":"value"
// Returnează pointer la value (null-terminat pe loc) sau nullptr
static char* findJsonStr(char* json, const char* key) {
    // Construieste "key":
    char needle[20];
    byte klen = strlen(key);
    if (klen + 4 >= sizeof(needle)) return nullptr;
    needle[0] = '"';
    memcpy(needle + 1, key, klen);
    needle[klen + 1] = '"';
    needle[klen + 2] = ':';
    needle[klen + 3] = '"';
    needle[klen + 4] = '\0';

    char* p = strstr(json, needle);
    if (!p) return nullptr;
    p += klen + 4;  // sare peste "key":"

    // Gaseste inchiderea "
    char* end = strchr(p, '"');
    if (!end) return nullptr;
    *end = '\0';  // termina sirul pe loc
    return p;
}

// Cauta valoarea unui camp nested: "arg":{"uid":"value"}
// Returneaza pointer la value sau nullptr
static char* findNestedStr(char* json, const char* outerKey, const char* innerKey) {
    char needle[20];
    byte klen = strlen(outerKey);
    if (klen + 5 >= sizeof(needle)) return nullptr;
    needle[0] = '"';
    memcpy(needle + 1, outerKey, klen);
    needle[klen + 1] = '"';
    needle[klen + 2] = ':';
    needle[klen + 3] = '{';
    needle[klen + 4] = '\0';

    char* p = strstr(json, needle);
    if (!p) return nullptr;
    p += klen + 4;

    // Gaseste inchiderea }
    char* end = strchr(p, '}');
    if (!end) return nullptr;
    char saved = *end;
    *end = '\0';

    char* result = findJsonStr(p, innerKey);
    *end = saved;  // restaureaza
    return result;
}

static void processCommand(char* json) {
    if (!s_cmdCb) return;

    Serial.print(F("[MQTT RX] raw="));
    Serial.println(json);

    // Detecteaza formatul: Live Objects ("req") sau legacy ("cmd")
    char* req = findJsonStr(json, "req");
    if (!req) req = findJsonStr(json, "cmd");
    if (!req) {
        Serial.println(F("[CMD] nu gasesc req/cmd"));
        return;
    }

    Serial.print(F("[CMD] req="));
    Serial.println(req);

    NbiotCommand command;
    memset(&command, 0, sizeof(command));

    if (strcmp(req, "open") == 0) {
        command.type = CMD_OPEN;

    } else if (strcmp(req, "status") == 0) {
        command.type = CMD_STATUS;

    } else if (strcmp(req, "card_add") == 0) {
        command.type = CMD_CARD_ADD;

        // Incearca Live Objects: arg.uid
        char* uid = findNestedStr(json, "arg", "uid");
        // Fallback legacy: uid direct
        if (!uid) uid = findJsonStr(json, "uid");

        if (!uid || uid[0] == '\0') {
            Serial.println(F("[CMD] card_add: uid lipsa"));
            return;
        }
        strncpy(command.uid, uid, sizeof(command.uid) - 1);
        Serial.print(F("[CMD] uid="));
        Serial.println(command.uid);

    } else if (strcmp(req, "card_remove") == 0) {
        command.type = CMD_CARD_REMOVE;

        char* uid = findNestedStr(json, "arg", "uid");
        if (!uid) uid = findJsonStr(json, "uid");

        if (!uid || uid[0] == '\0') {
            Serial.println(F("[CMD] card_remove: uid lipsa"));
            return;
        }
        strncpy(command.uid, uid, sizeof(command.uid) - 1);
        Serial.print(F("[CMD] uid="));
        Serial.println(command.uid);

    } else {
        Serial.print(F("[CMD] necunoscut: "));
        Serial.println(req);
        return;
    }

    s_cmdCb(command);
}

// ============================================================
//  nbiot_checkCommands() — citire non-blocanta + procesare
//
//  Apelati cat mai des in loop().
//  Citeste prin modemRead() (rute +QMTRECV in cmdBuf).
//  Proceseaza o comanda per apel cand linia e completa.
// ============================================================
void nbiot_checkCommands() {
    modemRead();

    if (!s_cmdReady) return;
    s_cmdReady = false;

    // Gaseste JSON-ul din linia +QMTRECV: 0,0,dev/cmd,{...}
    // Format: +QMTRECV: <clientIdx>,<msgId>,<topic>,<payload>
    char* jsonStart = strchr(s_cmdBuf, '{');
    if (!jsonStart) {
        // Linie incompleta sau fara JSON — reseteaza si asteapta mai mult
        // Daca mai avem date, le pastram; daca nu, resetam
        if (!strstr(s_cmdBuf, "+QMTRECV")) {
            s_cmdPos = 0;
            s_cmdBuf[0] = '\0';
        }
        return;
    }

    // Gaseste sfarsitul JSON-ului
    char* jsonEnd = strrchr(jsonStart, '}');
    if (!jsonEnd) {
        // JSON incomplet — mai asteaptam date
        // Daca bufferul e aproape plin, resetam
        if (s_cmdPos >= CMD_BUF_SIZE - 5) {
            Serial.println(F("[CMD] buf overflow, reset"));
            s_cmdPos = 0;
            s_cmdBuf[0] = '\0';
        }
        s_cmdReady = false;  // nu e gata, mai asteptam
        return;
    }

    *(jsonEnd + 1) = '\0';  // termina dupa }

    processCommand(jsonStart);

    // Reset buffer dupa procesare
    s_cmdPos = 0;
    s_cmdBuf[0] = '\0';
}

void nbiot_setCommandCallback(NbiotCommandCallback cb) { s_cmdCb = cb; }

bool nbiot_isConnected() { return s_connected; }