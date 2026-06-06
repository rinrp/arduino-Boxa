#include <Arduino.h>
#include "config.h"
#include "sensors.h"
#include "outputs.h"
#include "access.h"
#include "cardmanager.h"   // ← nou: gestiune carduri EEPROM
#include "nbiot.h"         // ← nou: NB-IoT + MQTT Live Objects


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
static char          g_lastUID[15]       = "";  // UID-ul cardului curent

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


// ============================================================
//  Helper: tranziție cu log
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
//  Apelat din nbiot_checkCommands() când sosește o comandă.
// ============================================================

static void onNbiotCommand(const NbiotCommand& cmd) {
    switch (cmd.type) {

        // ----------------------------------------------------------
        // CMD_OPEN: deschide yala de la distanță
        // Echivalent cu apăsarea butonului fizic, dar numai din IDLE.
        // ----------------------------------------------------------
        case CMD_OPEN:
            Serial.println(F("[CMD] Remote: DESCHIDE USA"));
            nbiot_publish(EVT_REMOTE_OPEN, "", stateNameStr(g_state));

            if (g_state == IDLE) {
                g_remoteOpenPending = true;  // Procesat în loop()
            } else {
                Serial.println(F("[CMD] Ignorat: sistemul nu e in IDLE"));
            }
            break;

        // ----------------------------------------------------------
        // CMD_CARD_ADD: adaugă un card nou în EEPROM
        // ----------------------------------------------------------
        case CMD_CARD_ADD: {
            Serial.print(F("[CMD] Remote: ADAUGA CARD "));
            Serial.println(cmd.uid);

            byte uid[4];
            if (cardmanager_parseUID(cmd.uid, uid)) {
                bool ok = cardmanager_add(uid);
                // Publică confirmare
                nbiot_publish(ok ? EVT_CARD_ADDED : EVT_SYSTEM_STATUS,
                              cmd.uid,
                              ok ? "added" : "add_failed");
            } else {
                Serial.println(F("[CMD] card_add: UID invalid"));
                nbiot_publish(EVT_SYSTEM_STATUS, cmd.uid, "uid_parse_error");
            }
            break;
        }

        // ----------------------------------------------------------
        // CMD_CARD_REMOVE: elimină un card din EEPROM
        // ----------------------------------------------------------
        case CMD_CARD_REMOVE: {
            Serial.print(F("[CMD] Remote: ELIMINA CARD "));
            Serial.println(cmd.uid);

            byte uid[4];
            if (cardmanager_parseUID(cmd.uid, uid)) {
                bool ok = cardmanager_remove(uid);
                nbiot_publish(ok ? EVT_CARD_REMOVED : EVT_SYSTEM_STATUS,
                              cmd.uid,
                              ok ? "removed" : "not_found");
            } else {
                Serial.println(F("[CMD] card_remove: UID invalid"));
                nbiot_publish(EVT_SYSTEM_STATUS, cmd.uid, "uid_parse_error");
            }
            break;
        }

        // ----------------------------------------------------------
        // CMD_STATUS: publică starea curentă
        // ----------------------------------------------------------
        case CMD_STATUS:
            Serial.println(F("[CMD] Remote: STATUS REQUEST"));
            nbiot_publish(EVT_SYSTEM_STATUS, "", stateNameStr(g_state));
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

    // Inițializări hardware
    access_init();
    outputs_init();
    sensors_init();

    // Gestiune carduri din EEPROM
    cardmanager_init();

    // NB-IoT — înregistrăm callback-ul ÎNAINTE de init
    nbiot_setCommandCallback(onNbiotCommand);
    if (!nbiot_init()) {
        Serial.println(F("AVERTIZARE: NB-IoT offline. Sistem functional local."));
        // Sistemul continuă să funcționeze local chiar fără conexiune cloud
    }

    led_allOff();

    Serial.println(F("Initializare completa"));
    Serial.println();
}


// ============================================================
//  loop()
// ============================================================

void loop() {
    unsigned long now = millis();

    // ----------------------------------------------------------
    // 1. NB-IoT — inițializare în background + comenzi
    //    initTick() finalizează conectarea la rețea/MQTT fără
    //    a bloca senzorii sau RFID-ul
    // ----------------------------------------------------------
    nbiot_initTick();
    nbiot_checkCommands();

    // ----------------------------------------------------------
    // 2. Heartbeat periodic către Live Objects
    // ----------------------------------------------------------
    nbiot_heartbeatTick(stateNameStr(g_state));

    // ----------------------------------------------------------
    // 3. Detectare schimbare reed (debug serial)
    // ----------------------------------------------------------
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

        // Publică evenimentul de schimbare stare ușă
        nbiot_publish(
            isOpen ? EVT_DOOR_OPEN : EVT_DOOR_CLOSED,
            "",
            stateNameStr(g_state)
        );
    }

    // ----------------------------------------------------------
    // 4. Log periodic stare ușă
    // ----------------------------------------------------------
    if (now - g_lastLogMs >= LOG_INTERVAL_MS) {
        g_lastLogMs = now;
        door_printState();
    }

    // ----------------------------------------------------------
    // 5. Perioadă de stabilizare post-închidere
    // ----------------------------------------------------------
    if (g_stabilizing) {
        if (now - g_stabilizeStartMs >= DURATA_STABILIZARE_MS) {
            g_stabilizing = false;
            led_allOff();
            Serial.println(F("Sistem ARMAT"));
        } else if (now - g_stabilizeStartMs > STABILIZARE_MAX_MS) {
            g_stabilizing = false;
            Serial.println(F("AVERTIZARE: stabilizare reset fortat"));
        }
        return;
    }

    // ----------------------------------------------------------
    // 6. State machine
    // ----------------------------------------------------------

    switch (g_state) {

        // ======================================================
        case IDLE:
        // ======================================================

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

                // Verificare cu cardmanager (EEPROM) în loc de UID hardcodat
                if (cardmanager_isValid(access_getUID())) {
                    Serial.println(F("Card: VALID - acces permis"));
                    led_greenOn();
                    buzzer_confirmSound();

                    nbiot_publish(EVT_ACCESS_GRANTED,
                                  g_lastUID,
                                  "ACCESS_GRANTED");

                    g_yalaActive        = false;
                    g_doorOpenedInCycle = false;
                    enterState(ACCESS_GRANTED);
                } else {
                    Serial.println(F("Card: INVALID - acces refuzat"));
                    led_redOn();
                    buzzer_errorSound();

                    nbiot_publish(EVT_ACCESS_DENIED,
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

                nbiot_publish(EVT_ALARM_START, "", "ALARM");

                enterState(ALARM);
            }

            break;


        // ======================================================
        case ACCESS_GRANTED:
        // ======================================================

            led_greenOn();

            if (!g_yalaActive &&
                now - g_stateEntryMs >= DELAY_YALA_MS) {
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
                    now - g_yalaActivatedMs >= TIMEOUT_ASTEPTARE_USA_MS) {
                    Serial.println(F("Timeout: usa nu s-a deschis"));
                    led_allOff();
                    g_yalaActive = false;
                    enterState(IDLE);
                    break;
                }
            }

            break;


        // ======================================================
        case ACCESS_DENIED:
        // ======================================================

            led_redOn();

            if (now - g_stateEntryMs >= DURATA_REFUZ_CARD_MS) {
                Serial.println(F("Card invalid: timeout -> IDLE"));
                led_redOff();
                enterState(IDLE);
            }

            if (door_isOpen()) {
                Serial.println(F("ALARMA: Usa fortata in ACCESS_DENIED!"));
                buzzer_alarmStart();

                nbiot_publish(EVT_ALARM_START, g_lastUID, "ALARM");

                enterState(ALARM);
            }

            break;


        // ======================================================
        case TEMP_OPEN:
        // ======================================================

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


        // ======================================================
        case ALARM:
        // ======================================================

            led_redOn();
            buzzer_alarmTick();

            if (door_isClosed()) {
                Serial.println(F("Usa inchisa -> alarma oprita"));
                buzzer_alarmStop();
                led_redOff();

                nbiot_publish(EVT_ALARM_STOP, "", "IDLE");

                enterState(IDLE);
            }

            break;

    } // switch
}