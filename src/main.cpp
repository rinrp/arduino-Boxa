#include <Arduino.h>
#include "config.h"
#include "sensors.h"
#include "outputs.h"
#include "access.h"
#include "cardmanager.h"
#include "nbiot.h"
#include "diagnostics.h"


// ============================================================
//  Definiție stări
// ============================================================

enum State {
    IDLE,
    ACCESS_GRANTED,
    ACCESS_DENIED,
    TEMP_OPEN,
    ALARM
};


// ============================================================
//  Variabile globale state machine
// ============================================================

static State         g_state             = IDLE;
static unsigned long g_stateEntryMs      = 0;

// ACCESS_GRANTED
static bool          g_yalaActive        = false;
static unsigned long g_yalaActivatedMs   = 0;
static bool          g_doorOpenedInCycle = false;
static char          g_lastUID[15]       = "";

// TEMP_OPEN
static bool          g_doorOpenedByBtn   = false;

// Stabilizare la închiderea ușii
static bool          g_stabilizing       = false;
static unsigned long g_stabilizeStartMs  = 0;

// Log periodic reed
static unsigned long g_lastLogMs         = 0;
static int           g_prevReedRaw       = -1;

// Deschidere remotă comandată din cloud
static bool          g_remoteOpenPending = false;

// NB-IoT service throttling
static unsigned long g_lastNbiotTickMs   = 0;
static unsigned long g_lastHeartbeatMs   = 0;


// ============================================================
//  Coadă simplă pentru evenimente NB-IoT
//  Scop: nu apelăm nbiot_publish() direct în mijlocul RFID/alarmă.
// ============================================================

#define EVENT_QUEUE_SIZE 5
#define EVENT_NAME_MAX   24
#define EVENT_UID_MAX    15
#define EVENT_STATE_MAX  20

struct PendingEvent {
    char event[EVENT_NAME_MAX];
    char uid[EVENT_UID_MAX];
    char state[EVENT_STATE_MAX];
};

static PendingEvent g_eventQueue[EVENT_QUEUE_SIZE];
static byte g_eventHead = 0;
static byte g_eventTail = 0;
static byte g_eventCount = 0;

static bool queueEvent(const char* eventType,
                       const char* uid,
                       const char* stateStr)
{
    if (g_eventCount >= EVENT_QUEUE_SIZE) {
        Serial.print(F("[QUEUE] Plina, eveniment pierdut: "));
        Serial.println(eventType);
        return false;
    }

    strncpy(g_eventQueue[g_eventTail].event, eventType ? eventType : "", EVENT_NAME_MAX - 1);
    g_eventQueue[g_eventTail].event[EVENT_NAME_MAX - 1] = '\0';

    strncpy(g_eventQueue[g_eventTail].uid, uid ? uid : "", EVENT_UID_MAX - 1);
    g_eventQueue[g_eventTail].uid[EVENT_UID_MAX - 1] = '\0';

    strncpy(g_eventQueue[g_eventTail].state, stateStr ? stateStr : "", EVENT_STATE_MAX - 1);
    g_eventQueue[g_eventTail].state[EVENT_STATE_MAX - 1] = '\0';

    g_eventTail = (g_eventTail + 1) % EVENT_QUEUE_SIZE;
    g_eventCount++;

    return true;
}

static bool popEvent(PendingEvent& out)
{
    if (g_eventCount == 0) return false;

    out = g_eventQueue[g_eventHead];

    g_eventHead = (g_eventHead + 1) % EVENT_QUEUE_SIZE;
    g_eventCount--;

    return true;
}


// ============================================================
//  Helper: nume stare
// ============================================================

static const __FlashStringHelper* stateName(State s) {
    switch (s) {
        case IDLE:           return F("IDLE");
        case ACCESS_GRANTED: return F("ACCESS_GRANTED");
        case ACCESS_DENIED:  return F("ACCESS_DENIED");
        case TEMP_OPEN:      return F("TEMP_OPEN");
        case ALARM:          return F("ALARM");
        default:             return F("?");
    }
}

static const char* stateNameStr(State s) {
    switch (s) {
        case IDLE:           return "IDLE";
        case ACCESS_GRANTED: return "ACCESS_GRANTED";
        case ACCESS_DENIED:  return "ACCESS_DENIED";
        case TEMP_OPEN:      return "TEMP_OPEN";
        case ALARM:          return "ALARM";
        default:             return "UNKNOWN";
    }
}

static void enterState(State next) {
    if (next == g_state) return;

    Serial.print(F("STATE: "));
    Serial.print(stateName(g_state));
    Serial.print(F(" -> "));
    Serial.println(stateName(next));

    g_state        = next;
    g_stateEntryMs = millis();
}


// ============================================================
//  Helper: terminare ciclu cu stabilizare
// ============================================================

static void beginStabilization() {
    led_allOff();

    g_stabilizing      = true;
    g_stabilizeStartMs = millis();

    enterState(IDLE);

    Serial.println(F("Stabilizare activa..."));
}


// ============================================================
//  Callback comenzi NB-IoT
// ============================================================

static void onNbiotCommand(const NbiotCommand& cmd) {
    switch (cmd.type) {

        case CMD_OPEN:
            Serial.println(F("[CMD] Remote: DESCHIDE USA"));
            queueEvent(EVT_REMOTE_OPEN, "", stateNameStr(g_state));

            if (g_state == IDLE) {
                g_remoteOpenPending = true;
            } else {
                Serial.println(F("[CMD] Ignorat: sistemul nu e in IDLE"));
            }
            break;


        case CMD_CARD_ADD: {
            Serial.print(F("[CMD] Remote: ADAUGA CARD "));
            Serial.println(cmd.uid);

            byte uid[4];

            if (cardmanager_parseUID(cmd.uid, uid)) {
                bool ok = cardmanager_add(uid);

                queueEvent(ok ? EVT_CARD_ADDED : EVT_SYSTEM_STATUS,
                           cmd.uid,
                           ok ? "added" : "add_failed");
            } else {
                Serial.println(F("[CMD] card_add: UID invalid"));
                queueEvent(EVT_SYSTEM_STATUS, cmd.uid, "uid_parse_error");
            }

            break;
        }


        case CMD_CARD_REMOVE: {
            Serial.print(F("[CMD] Remote: ELIMINA CARD "));
            Serial.println(cmd.uid);

            byte uid[4];

            if (cardmanager_parseUID(cmd.uid, uid)) {
                bool ok = cardmanager_remove(uid);

                queueEvent(ok ? EVT_CARD_REMOVED : EVT_SYSTEM_STATUS,
                           cmd.uid,
                           ok ? "removed" : "not_found");
            } else {
                Serial.println(F("[CMD] card_remove: UID invalid"));
                queueEvent(EVT_SYSTEM_STATUS, cmd.uid, "uid_parse_error");
            }

            break;
        }


        case CMD_STATUS:
            Serial.println(F("[CMD] Remote: STATUS REQUEST"));
            queueEvent(EVT_SYSTEM_STATUS, "", stateNameStr(g_state));
            break;


        default:
            break;
    }
}


// ============================================================
//  setup()
// ============================================================

void setup() {
    Serial.begin(SERIAL_BAUD);
    while (!Serial);

    Serial.println(F("===================================="));
    Serial.println(F("  Sistem control acces RFID v3.0   "));
    Serial.println(F("  + NB-IoT / Orange Live Objects   "));
    Serial.println(F("===================================="));

    access_init();
    outputs_init();
    sensors_init();

    cardmanager_init();

    nbiot_setCommandCallback(onNbiotCommand);

    if (!nbiot_init()) {
        Serial.println(F("AVERTIZARE: NB-IoT offline. Sistem functional local."));
    }

    diagnostics_runHardwareCheck();

    led_allOff();

    Serial.println(F("Initializare completa"));
    Serial.println();
}


// ============================================================
//  loop()
// ============================================================

void loop() {
    unsigned long now = millis();

    // ==========================================================
    // 1. Ieșiri/alarmă locală — prioritate mare
    // ==========================================================

    if (g_state == ALARM) {
        buzzer_alarmTick();
    }


    // ==========================================================
    // 2. Detectare schimbare reed
    // ==========================================================

    int reedRaw = digitalRead(REED_PIN);

    if (reedRaw != g_prevReedRaw) {
        g_prevReedRaw = reedRaw;

        bool isOpen = door_isOpen();

        Serial.print(F("STATUS="));
        Serial.print(reedRaw);
        Serial.print(F("  USA="));
        Serial.print(isOpen ? F("DESCHISA") : F("INCHISA"));
        Serial.print(F("  state="));
        Serial.println(stateName(g_state));

        queueEvent(
            isOpen ? EVT_DOOR_OPEN : EVT_DOOR_CLOSED,
            "",
            stateNameStr(g_state)
        );
    }


    // ==========================================================
    // 3. Log periodic stare ușă
    // ==========================================================

    if (now - g_lastLogMs >= LOG_INTERVAL_MS) {
        g_lastLogMs = now;
        door_printState();
    }


    // ==========================================================
    // 4. Stabilizare post-închidere
    // ==========================================================

    if (g_stabilizing) {
        if (now - g_stabilizeStartMs >= DURATA_STABILIZARE_MS) {
            g_stabilizing = false;
            led_allOff();
            Serial.println(F("Sistem ARMAT"));
        } else if (now - g_stabilizeStartMs > STABILIZARE_MAX_MS) {
            g_stabilizing = false;
            Serial.println(F("AVERTIZARE: stabilizare reset fortat"));
        }

        // Chiar și în stabilizare lăsăm NB-IoT să respire rar.
        if (now - g_lastNbiotTickMs >= 250) {
            g_lastNbiotTickMs = now;
            nbiot_initTick();
            nbiot_checkCommands();
        }

        return;
    }


    // ==========================================================
    // 5. State machine local — RFID/senzori înainte de NB-IoT
    // ==========================================================

    switch (g_state) {

        // ------------------------------------------------------
        case IDLE:
        // ------------------------------------------------------

            led_allOff();

            // --- Deschidere remotă din cloud ---
            if (g_remoteOpenPending) {
                g_remoteOpenPending = false;

                Serial.println(F("Remote: deschidere autorizata din cloud"));

                led_blueOn();
                g_doorOpenedByBtn = false;

                enterState(TEMP_OPEN);
                break;
            }


            // --- Card RFID ---
            if (access_isCardDetected()) {
                access_printUID();
                access_uidToString(g_lastUID, sizeof(g_lastUID));

                if (cardmanager_isValid(access_getUID())) {
                    Serial.println(F("Card: VALID - acces permis"));

                    led_greenOn();
                    buzzer_confirmSound();

                    queueEvent(EVT_ACCESS_GRANTED,
                               g_lastUID,
                               "ACCESS_GRANTED");

                    g_yalaActive        = false;
                    g_doorOpenedInCycle = false;

                    enterState(ACCESS_GRANTED);
                } else {
                    Serial.println(F("Card: INVALID - acces refuzat"));

                    led_redOn();
                    buzzer_errorSound();

                    queueEvent(EVT_ACCESS_DENIED,
                               g_lastUID,
                               "ACCESS_DENIED");

                    enterState(ACCESS_DENIED);
                }

                access_stopCommunication();
            }


            // --- Buton deschidere manuală ---
            if (button_wasPressed()) {
                Serial.println(F("Buton: deschidere manuala"));

                led_blueOn();
                g_doorOpenedByBtn = false;

                enterState(TEMP_OPEN);
            }


            // --- Ușă forțată ---
            if (door_isOpen()) {
                Serial.println(F("ALARMA: Usa fortata!"));

                led_redOn();
                buzzer_alarmStart();

                enterState(ALARM);

                queueEvent(EVT_ALARM_START, "", "ALARM");
            }

            break;


        // ------------------------------------------------------
        case ACCESS_GRANTED:
        // ------------------------------------------------------

            led_greenOn();

            if (!g_yalaActive &&
                now - g_stateEntryMs >= DELAY_YALA_MS)
            {
                led_blueOn();

                g_yalaActive        = true;
                g_yalaActivatedMs   = now;
                g_doorOpenedInCycle = false;

                Serial.println(F("Yala activa (LED albastru)"));
            }

            if (g_yalaActive) {
                if (door_isOpen()) {
                    g_doorOpenedInCycle = true;
                }

                if (g_doorOpenedInCycle && door_isClosed()) {
                    Serial.println(F("Usa inchisa dupa acces valid -> stabilizare"));

                    beginStabilization();

                    g_yalaActive        = false;
                    g_doorOpenedInCycle = false;

                    break;
                }

                if (!g_doorOpenedInCycle &&
                    now - g_yalaActivatedMs >= TIMEOUT_ASTEPTARE_USA_MS)
                {
                    Serial.println(F("Timeout: usa nu s-a deschis"));

                    led_allOff();

                    g_yalaActive = false;

                    enterState(IDLE);

                    break;
                }
            }

            break;


        // ------------------------------------------------------
        case ACCESS_DENIED:
        // ------------------------------------------------------

            led_redOn();

            if (now - g_stateEntryMs >= DURATA_REFUZ_CARD_MS) {
                Serial.println(F("Card invalid: timeout -> IDLE"));

                led_redOff();

                enterState(IDLE);
            }

            if (door_isOpen()) {
                Serial.println(F("ALARMA: Usa fortata in ACCESS_DENIED!"));

                led_redOn();
                buzzer_alarmStart();

                queueEvent(EVT_ALARM_START, g_lastUID, "ALARM");

                enterState(ALARM);
            }

            break;


        // ------------------------------------------------------
        case TEMP_OPEN:
        // ------------------------------------------------------

            led_blueOn();
            buzzer_buttonTick(now, g_stateEntryMs);

            if (door_isOpen()) {
                g_doorOpenedByBtn = true;
            }

            if (g_doorOpenedByBtn && door_isClosed()) {
                Serial.println(F("Usa inchisa dupa buton"));

                noTone(BUZZER_PIN);

                beginStabilization();

                g_doorOpenedByBtn = false;

                break;
            }

            if (now - g_stateEntryMs >= TIMEOUT_BUTON_MS) {
                Serial.println(F("Buton timeout: usa neutilizata"));

                noTone(BUZZER_PIN);
                led_blueOff();

                g_doorOpenedByBtn = false;

                enterState(IDLE);
            }

            break;


        // ------------------------------------------------------
        case ALARM:
        // ------------------------------------------------------

            led_redOn();
            buzzer_alarmTick();

            if (door_isClosed()) {
                Serial.println(F("Usa inchisa -> alarma oprita"));

                buzzer_alarmStop();
                led_redOff();

                queueEvent(EVT_ALARM_STOP, "", "IDLE");

                enterState(IDLE);
            }

            break;
    }


    // ==========================================================
    // 6. NB-IoT / MQTT — la final, rulat rar
    // ==========================================================

    if (now - g_lastNbiotTickMs >= 250) {
        g_lastNbiotTickMs = now;

        nbiot_initTick();
        nbiot_checkCommands();
    }


    // ==========================================================
    // 7. Heartbeat — nu în timpul alarmei
    // ==========================================================

    if (g_state != ALARM &&
        now - g_lastHeartbeatMs >= NBIOT_HEARTBEAT_MS)
    {
        g_lastHeartbeatMs = now;
        queueEvent(EVT_SYSTEM_STATUS, "", stateNameStr(g_state));
    }


    // ==========================================================
    // 8. Publicare evenimente — unul pe iterație controlată
    //    Atenție: nbiot_publish poate bloca, de aceea este la final.
    // ==========================================================

    static unsigned long lastPublishAttemptMs = 0;

    if (now - lastPublishAttemptMs >= 1000) {
        lastPublishAttemptMs = now;

        PendingEvent ev;

        if (popEvent(ev)) {
            nbiot_publish(ev.event, ev.uid, ev.state);
        }
    }
}