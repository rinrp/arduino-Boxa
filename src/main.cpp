#include <Arduino.h>
#include "config.h"
#include "sensors.h"
#include "outputs.h"
#include "access.h"

// ============================================================
//  main.cpp — State machine sistem control acces
//
//  Stări:
//    IDLE          → sistem armat, monitorizare normală
//    ACCESS_GRANTED → card valid, yală activată
//    ACCESS_DENIED  → card invalid, semnal eroare
//    TEMP_OPEN      → deschidere manuală prin buton
//    ALARM          → ușă forțată, alarmă activă
// ============================================================


  
//  Definiție stări
  

enum State {
    IDLE,
    ACCESS_GRANTED,
    ACCESS_DENIED,
    TEMP_OPEN,
    ALARM
};


  
//  Variabile globale state machine
  

static State         g_state             = IDLE;
static unsigned long g_stateEntryMs      = 0;   // millis() la intrarea în stare

// ACCESS_GRANTED
static bool          g_yalaActive        = false;  // true după delay yală
static unsigned long g_yalaActivatedMs   = 0;
static bool          g_doorOpenedInCycle = false;

// TEMP_OPEN
static bool          g_doorOpenedByBtn   = false;

// Stabilizare la închiderea ușii
static bool          g_stabilizing       = false;
static unsigned long g_stabilizeStartMs  = 0;

// Log periodic reed
static unsigned long g_lastLogMs         = 0;
static int           g_prevReedRaw       = -1;


  
//  Helper: tranziție cu log
  

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

static void enterState(State next) {
    if (next == g_state) return;
    Serial.print(F("STATE: "));
    Serial.print(stateName(g_state));
    Serial.print(F(" -> "));
    Serial.println(stateName(next));
    g_state        = next;
    g_stateEntryMs = millis();
}


  
//  Helper: terminare ciclu cu stabilizare
//  Apelat când ușa s-a deschis ȘI s-a închis corect.
  

static void beginStabilization() {
    led_allOff();
    g_stabilizing       = true;
    g_stabilizeStartMs  = millis();
    enterState(IDLE);
    Serial.println(F("Stabilizare 2s activa..."));
}


// ============================================================
//  setup()
// ============================================================

void setup() {
    Serial.begin(SERIAL_BAUD);
    while (!Serial);

    Serial.println(F("===================================="));
    Serial.println(F("  Sistem control acces RFID v2.0   "));
    Serial.println(F("===================================="));

    access_init();
    outputs_init();
    sensors_init();

    led_allOff();

    Serial.println(F("Initializare completa. Stare: IDLE"));
    Serial.println();
}


// ============================================================
//  loop()
// ============================================================

void loop() {
    unsigned long now = millis();

    // ----------------------------------------------------------
    // 1. Detectare schimbare reed (debug serial)
    // ----------------------------------------------------------
    int reedRaw = digitalRead(REED_PIN);
    if (reedRaw != g_prevReedRaw) {
        g_prevReedRaw = reedRaw;
        Serial.print(F("REED raw="));
        Serial.print(reedRaw);
        Serial.print(F("  usa="));
        Serial.print(door_isOpen() ? F("DESCHISA") : F("INCHISA"));
        Serial.print(F("  state="));
        Serial.println(stateName(g_state));
    }

    // ----------------------------------------------------------
    // 2. Log periodic stare ușă
    // ----------------------------------------------------------
    if (now - g_lastLogMs >= LOG_INTERVAL_MS) {
        g_lastLogMs = now;
        door_printState();
    }

    // ----------------------------------------------------------
    // 3. Perioadă de stabilizare post-închidere
    //    Ignoră orice eveniment de alarmă în această fereastră.
    // ----------------------------------------------------------
    if (g_stabilizing) {
        if (now - g_stabilizeStartMs >= DURATA_STABILIZARE_MS) {
            g_stabilizing = false;
            led_allOff();
            Serial.println(F("Sistem ARMAT"));
        } else if (now - g_stabilizeStartMs > STABILIZARE_MAX_MS) {
            // Watchdog de siguranță
            g_stabilizing = false;
            Serial.println(F("WARN: stabilizare reset fortat"));
        }
        return;  // <-- NU procesăm nicio stare în timpul stabilizării
    }

    // ----------------------------------------------------------
    // 4. State machine
    // ----------------------------------------------------------

    switch (g_state) {

        // ======================================================
        case IDLE:
        // ======================================================
        //  LED-uri: toate stinse
        //  Monitorizează: card RFID, buton, ușă forțată
        // ======================================================

            led_allOff();

            // --- Card RFID ---
            if (access_isCardDetected()) {
                access_printUID();

                if (access_isUIDValid()) {
                    // Card valid: LED verde + bip confirmare → ACCESS_GRANTED
                    Serial.println(F("Card: VALID - acces permis"));
                    led_greenOn();
                    buzzer_confirmSound();

                    g_yalaActive        = false;
                    g_doorOpenedInCycle = false;
                    enterState(ACCESS_GRANTED);
                } else {
                    // Card invalid: LED roșu + bip eroare → ACCESS_DENIED
                    Serial.println(F("Card: INVALID - acces refuzat"));
                    led_redOn();
                    buzzer_errorSound();
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

            // --- Ușă forțată (alarmă) ---
            // Verificăm DUPĂ stabilizare (return mai sus garantează asta)
            if (door_isOpen()) {
                Serial.println(F("ALARMA: usa fortata in IDLE!"));
                led_redOn();
                buzzer_alarmStart();
                enterState(ALARM);
            }

            break;


        // ======================================================
        case ACCESS_GRANTED:
        // ======================================================
        //  Faza 1 (0 → DELAY_YALA_MS):     LED verde aprins
        //  Faza 2 (DELAY_YALA_MS → +):     LED verde + albastru (yală activă)
        //    → dacă ușa se deschide și se închide: stabilizare → IDLE
        //    → dacă timeout TIMEOUT_ASTEPTARE_USA_MS fără deschidere: IDLE
        //    → dacă ușa e forțată: ALARM
        // ======================================================

            led_greenOn();

            // Faza 1→2: activare yală după delay
            if (!g_yalaActive &&
                now - g_stateEntryMs >= DELAY_YALA_MS) {
                led_blueOn();
                g_yalaActive      = true;
                g_yalaActivatedMs = now;
                g_doorOpenedInCycle = false;
                Serial.println(F("Yala activa (LED albastru)"));
            }

            if (g_yalaActive) {
                // Urmărim dacă ușa s-a deschis
                if (door_isOpen()) {
                    g_doorOpenedInCycle = true;
                }

                // Ușă deschisă → închisă: acces completat
                if (g_doorOpenedInCycle && door_isClosed()) {
                    Serial.println(F("Usa inchisa dupa acces valid -> stabilizare"));
                    beginStabilization();
                    g_yalaActive        = false;
                    g_doorOpenedInCycle = false;
                    break;
                }

                // Timeout: ușa nu s-a deschis deloc
                if (!g_doorOpenedInCycle &&
                    now - g_yalaActivatedMs >= TIMEOUT_ASTEPTARE_USA_MS) {
                    Serial.println(F("Timeout: usa nu s-a deschis -> IDLE"));
                    led_allOff();
                    g_yalaActive = false;
                    enterState(IDLE);
                    break;
                }

                // Ușă forțată (yală era activă dar ușa n-a trebuit să fie deschisă)
                // Acoperit implicit: dacă doorOpenedInCycle e true și ușa rămâne
                // deschisă mai mult decât normal, nu declanșăm alarmă — e o
                // deschidere autorizată în curs. Alarma se declanșează în IDLE.
            }

            break;


        // ======================================================
        case ACCESS_DENIED:
        // ======================================================
        //  LED roșu aprins DURATA_REFUZ_CARD_MS, apoi IDLE.
        //  Dacă ușa se deschide în acest interval: ALARM.
        // ======================================================

            led_redOn();

            // Timeout → re-armare
            if (now - g_stateEntryMs >= DURATA_REFUZ_CARD_MS) {
                Serial.println(F("Card invalid: timeout -> IDLE"));
                led_redOff();
                enterState(IDLE);
            }

            // Ușă forțată în timp ce cardul era invalid
            if (door_isOpen()) {
                Serial.println(F("ALARMA: usa fortata in ACCESS_DENIED!"));
                buzzer_alarmStart();
                enterState(ALARM);
            }

            break;


        // ======================================================
        case TEMP_OPEN:
        // ======================================================
        //  LED albastru + beep periodic TIMEOUT_BUTON_MS.
        //  Dacă ușa se deschide și se închide: stabilizare → IDLE.
        //  Dacă timeout fără deschidere: IDLE (re-armare automată).
        //  Nu se declanșează alarmă în TEMP_OPEN (deschidere autorizată).
        // ======================================================

            led_blueOn();
            buzzer_buttonTick(now, g_stateEntryMs);

            // Urmărim deschiderea ușii
            if (door_isOpen()) {
                g_doorOpenedByBtn = true;
            }

            // Ușă deschisă → închisă
            if (g_doorOpenedByBtn && door_isClosed()) {
                Serial.println(F("Usa inchisa dupa buton -> stabilizare"));
                noTone(BUZZER_PIN);
                beginStabilization();
                g_doorOpenedByBtn = false;
                break;
            }

            // Timeout fără utilizare
            if (now - g_stateEntryMs >= TIMEOUT_BUTON_MS) {
                Serial.println(F("Buton timeout: usa neutilizata -> IDLE"));
                noTone(BUZZER_PIN);
                led_blueOff();
                g_doorOpenedByBtn = false;
                enterState(IDLE);
            }

            break;


        // ======================================================
        case ALARM:
        // ======================================================
        //  LED roșu + buzzer intermitent.
        //  Oprire DOAR când ușa se închide.
        //  (Varianta extinsă: adăugați un PIN pentru cheie de dezarmare.)
        // ======================================================

            led_redOn();
            buzzer_alarmTick();

            if (door_isClosed()) {
                Serial.println(F("Usa inchisa -> alarma oprita -> IDLE"));
                buzzer_alarmStop();
                led_redOff();
                enterState(IDLE);
            }

            break;

    } // switch
}
