#include "sensors.h"

// ============================================================
//  sensors.cpp — Implementare senzori
// ============================================================

 
//  State intern buton
 
static bool     s_prevButtonState     = HIGH;   // HIGH = neapăsat (INPUT_PULLUP)
static unsigned long s_lastPressMs    = 0;


 
//  Inițializare
 

void sensors_init() {
    pinMode(REED_PIN,  INPUT_PULLUP);
    pinMode(BUTON_PIN, INPUT_PULLUP);
    Serial.println(F("Sensors: reed A0, buton A1 initializate"));
}


 
//  Reed switch
 

bool door_isClosed() {
    return digitalRead(REED_PIN) == DOOR_CLOSED_STATE;
}

bool door_isOpen() {
    return digitalRead(REED_PIN) == DOOR_OPEN_STATE;
}

void door_printState() {
    if (door_isClosed()) {
        Serial.println(F("Reed: usa INCHISA"));
    } else {
        Serial.println(F("Reed: usa DESCHISA"));
    }
}


 
//  Buton cu debouncing
 

bool button_wasPressed() {
    bool current = digitalRead(BUTON_PIN);

    // Front descendent (HIGH→LOW) cu debouncing
    if (current == LOW && s_prevButtonState == HIGH) {
        if (millis() - s_lastPressMs > BUTON_DEBOUNCE_MS) {
            s_lastPressMs    = millis();
            s_prevButtonState = current;
            Serial.println(F("Buton: apasat"));
            return true;
        }
    }

    s_prevButtonState = current;
    return false;
}
