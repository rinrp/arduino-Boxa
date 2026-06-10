#include <Arduino.h>
#include "config.h"
#include "sensors.h"
#include "outputs.h"
#include "access.h"
#include "cardmanager.h"
#include "nbiot.h"
#include "diagnostics.h"

enum State : byte {
    IDLE,
    ACCESS_GRANTED,
    ACCESS_DENIED,
    TEMP_OPEN,
    ALARM
};

enum EventCode : byte {
    EV_NONE,
    EV_ACCESS_GRANTED,
    EV_ACCESS_DENIED,
    EV_DOOR_OPEN,
    EV_DOOR_CLOSED,
    EV_ALARM_START,
    EV_ALARM_STOP,
    EV_SYSTEM_STATUS,
    EV_CARD_ADDED,
    EV_CARD_REMOVED,
    EV_REMOTE_OPEN
};

#define EVENT_QUEUE_SIZE 4

struct PendingEvent {
    EventCode code;
    State state;
    char uid[15];
};

static State g_state = IDLE;
static unsigned long g_stateEntryMs = 0;

static bool g_yalaActive = false;
static unsigned long g_yalaActivatedMs = 0;
static bool g_doorOpenedInCycle = false;

static char g_lastUID[15] = "";

static bool g_doorOpenedByBtn = false;

static bool g_stabilizing = false;
static unsigned long g_stabilizeStartMs = 0;

static unsigned long g_lastLogMs = 0;
static int g_prevReedRaw = -1;

static bool g_remoteOpenPending = false;

static unsigned long g_lastNbiotTickMs = 0;
static unsigned long g_lastPublishAttemptMs = 0;
static unsigned long g_lastHeartbeatQueueMs = 0;

static PendingEvent g_queue[EVENT_QUEUE_SIZE];
static byte g_qHead = 0;
static byte g_qTail = 0;
static byte g_qCount = 0;

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
        default:             return "?";
    }
}

static const char* eventName(EventCode e) {
    switch (e) {
        case EV_ACCESS_GRANTED: return EVT_ACCESS_GRANTED;
        case EV_ACCESS_DENIED:  return EVT_ACCESS_DENIED;
        case EV_DOOR_OPEN:      return EVT_DOOR_OPEN;
        case EV_DOOR_CLOSED:    return EVT_DOOR_CLOSED;
        case EV_ALARM_START:    return EVT_ALARM_START;
        case EV_ALARM_STOP:     return EVT_ALARM_STOP;
        case EV_SYSTEM_STATUS:  return EVT_SYSTEM_STATUS;
        case EV_CARD_ADDED:     return EVT_CARD_ADDED;
        case EV_CARD_REMOVED:   return EVT_CARD_REMOVED;
        case EV_REMOTE_OPEN:    return EVT_REMOTE_OPEN;
        default:                return "";
    }
}

static void enterState(State next) {
    if (next == g_state) return;

    Serial.print(F("STATE: "));
    Serial.print(stateName(g_state));
    Serial.print(F(" -> "));
    Serial.println(stateName(next));

    g_state = next;
    g_stateEntryMs = millis();
}

static bool queueEvent(EventCode code, const char* uid, State state) {
    if (code == EV_NONE) return false;

    if (g_qCount >= EVENT_QUEUE_SIZE) {
        Serial.println(F("[Q] full"));
        return false;
    }

    g_queue[g_qTail].code = code;
    g_queue[g_qTail].state = state;

    strncpy(g_queue[g_qTail].uid, uid ? uid : "", sizeof(g_queue[g_qTail].uid) - 1);
    g_queue[g_qTail].uid[sizeof(g_queue[g_qTail].uid) - 1] = '\0';

    g_qTail = (g_qTail + 1) % EVENT_QUEUE_SIZE;
    g_qCount++;
    return true;
}

static bool popEvent(PendingEvent* out) {
    if (g_qCount == 0 || !out) return false;

    *out = g_queue[g_qHead];
    g_qHead = (g_qHead + 1) % EVENT_QUEUE_SIZE;
    g_qCount--;
    return true;
}

static void beginStabilization() {
    led_allOff();

    g_stabilizing = true;
    g_stabilizeStartMs = millis();

    enterState(IDLE);
    Serial.println(F("Stabilizare..."));
}

static void onNbiotCommand(const NbiotCommand& cmd) {
    switch (cmd.type) {
        case CMD_OPEN:
            Serial.println(F("[CMD] open"));
            queueEvent(EV_REMOTE_OPEN, "", g_state);

            if (g_state == IDLE) {
                g_remoteOpenPending = true;
            }
            break;

        case CMD_CARD_ADD: {
            byte uid[4];
            bool ok = cardmanager_parseUID(cmd.uid, uid) && cardmanager_add(uid);
            queueEvent(ok ? EV_CARD_ADDED : EV_SYSTEM_STATUS, cmd.uid, g_state);
            break;
        }

        case CMD_CARD_REMOVE: {
            byte uid[4];
            bool ok = cardmanager_parseUID(cmd.uid, uid) && cardmanager_remove(uid);
            queueEvent(ok ? EV_CARD_REMOVED : EV_SYSTEM_STATUS, cmd.uid, g_state);
            break;
        }

        case CMD_STATUS:
            queueEvent(EV_SYSTEM_STATUS, "", g_state);
            break;

        default:
            break;
    }
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    while (!Serial);

    Serial.println(F("===================================="));
    Serial.println(F("  Sistem control acces RFID v3.0"));
    Serial.println(F("  + NB-IoT / Live Objects"));
    Serial.println(F("===================================="));

    access_init();
    outputs_init();
    sensors_init();
    cardmanager_init();

    nbiot_setCommandCallback(onNbiotCommand);

    if (!nbiot_init()) {
        Serial.println(F("NB-IoT offline. Local OK."));
    }

#if RUN_HARDWARE_DIAGNOSTIC
    diagnostics_runHardwareCheck();
#endif

    led_allOff();

    Serial.println(F("Initializare completa"));
}

void loop() {
    unsigned long now = millis();

    if (g_state == ALARM) {
        buzzer_alarmTick();
    }

    int reedRaw = digitalRead(REED_PIN);

    if (reedRaw != g_prevReedRaw) {
        g_prevReedRaw = reedRaw;

        bool isOpen = door_isOpen();

        Serial.print(F("STATUS="));
        Serial.print(reedRaw);
        Serial.print(F(" USA="));
        Serial.print(isOpen ? F("DESCHISA") : F("INCHISA"));
        Serial.print(F(" state="));
        Serial.println(stateName(g_state));

        queueEvent(isOpen ? EV_DOOR_OPEN : EV_DOOR_CLOSED, "", g_state);
    }

    if (now - g_lastLogMs >= LOG_INTERVAL_MS) {
        g_lastLogMs = now;
        door_printState();
    }

    if (g_stabilizing) {
        if (now - g_stabilizeStartMs >= DURATA_STABILIZARE_MS) {
            g_stabilizing = false;
            led_allOff();
            Serial.println(F("Sistem ARMAT"));
        } else if (now - g_stabilizeStartMs > STABILIZARE_MAX_MS) {
            g_stabilizing = false;
            Serial.println(F("Stabilizare reset"));
        }
    } else {
        switch (g_state) {
            case IDLE:
                led_allOff();

                if (g_remoteOpenPending) {
                    g_remoteOpenPending = false;

                    Serial.println(F("Remote open"));
                    led_blueOn();
                    g_doorOpenedByBtn = false;
                    enterState(TEMP_OPEN);
                    break;
                }

                if (access_isCardDetected()) {
                    access_uidToString(g_lastUID, sizeof(g_lastUID));
                    access_printUID();

                    if (cardmanager_isValid(access_getUID())) {
                        Serial.println(F("Card VALID"));
                        led_greenOn();
                        buzzer_confirmSound();

                        queueEvent(EV_ACCESS_GRANTED, g_lastUID, ACCESS_GRANTED);

                        g_yalaActive = false;
                        g_doorOpenedInCycle = false;
                        enterState(ACCESS_GRANTED);
                    } else {
                        Serial.println(F("Card INVALID"));
                        led_redOn();
                        buzzer_errorSound();

                        queueEvent(EV_ACCESS_DENIED, g_lastUID, ACCESS_DENIED);
                        enterState(ACCESS_DENIED);
                    }

                    access_stopCommunication();
                }

                if (button_wasPressed()) {
                    Serial.println(F("Buton open"));
                    led_blueOn();
                    g_doorOpenedByBtn = false;
                    enterState(TEMP_OPEN);
                }

                if (door_isOpen()) {
                    Serial.println(F("ALARMA: usa fortata"));
                    led_redOn();
                    buzzer_alarmStart();

                    enterState(ALARM);
                    queueEvent(EV_ALARM_START, "", ALARM);
                }
                break;

            case ACCESS_GRANTED:
                led_greenOn();

                if (!g_yalaActive && now - g_stateEntryMs >= DELAY_YALA_MS) {
                    led_blueOn();
                    g_yalaActive = true;
                    g_yalaActivatedMs = now;
                    g_doorOpenedInCycle = false;
                    Serial.println(F("Yala activa"));
                }

                if (g_yalaActive) {
                    if (door_isOpen()) {
                        g_doorOpenedInCycle = true;
                    }

                    if (g_doorOpenedInCycle && door_isClosed()) {
                        Serial.println(F("Usa inchisa dupa acces"));
                        g_yalaActive = false;
                        g_doorOpenedInCycle = false;
                        beginStabilization();
                        break;
                    }

                    if (!g_doorOpenedInCycle &&
                        now - g_yalaActivatedMs >= TIMEOUT_ASTEPTARE_USA_MS)
                    {
                        Serial.println(F("Timeout usa"));
                        led_allOff();
                        g_yalaActive = false;
                        enterState(IDLE);
                    }
                }
                break;

            case ACCESS_DENIED:
                led_redOn();

                if (now - g_stateEntryMs >= DURATA_REFUZ_CARD_MS) {
                    Serial.println(F("Refuz timeout"));
                    led_redOff();
                    enterState(IDLE);
                }

                if (door_isOpen()) {
                    Serial.println(F("ALARMA dupa card invalid"));
                    led_redOn();
                    buzzer_alarmStart();

                    queueEvent(EV_ALARM_START, g_lastUID, ALARM);
                    enterState(ALARM);
                }
                break;

            case TEMP_OPEN:
                led_blueOn();
                buzzer_buttonTick(now, g_stateEntryMs);

                if (door_isOpen()) {
                    g_doorOpenedByBtn = true;
                }

                if (g_doorOpenedByBtn && door_isClosed()) {
                    Serial.println(F("Usa inchisa dupa buton"));
                    noTone(BUZZER_PIN);
                    g_doorOpenedByBtn = false;
                    beginStabilization();
                    break;
                }

                if (now - g_stateEntryMs >= TIMEOUT_BUTON_MS) {
                    Serial.println(F("Buton timeout"));
                    noTone(BUZZER_PIN);
                    led_blueOff();
                    g_doorOpenedByBtn = false;
                    enterState(IDLE);
                }
                break;

            case ALARM:
                led_redOn();
                buzzer_alarmTick();

                if (door_isClosed()) {
                    Serial.println(F("Alarma oprita"));
                    buzzer_alarmStop();
                    led_redOff();

                    queueEvent(EV_ALARM_STOP, "", IDLE);
                    enterState(IDLE);
                }
                break;
        }
    }

    if (now - g_lastNbiotTickMs >= 250) {
        g_lastNbiotTickMs = now;
        nbiot_initTick();
        nbiot_checkCommands();
    }

    if (g_state != ALARM && now - g_lastHeartbeatQueueMs >= NBIOT_HEARTBEAT_MS) {
        g_lastHeartbeatQueueMs = now;
        queueEvent(EV_SYSTEM_STATUS, "", g_state);
    }

    if (now - g_lastPublishAttemptMs >= 1200) {
        g_lastPublishAttemptMs = now;

        PendingEvent ev;
        if (popEvent(&ev)) {
            nbiot_publish(eventName(ev.code), ev.uid, stateNameStr(ev.state));
        }
    }
}
