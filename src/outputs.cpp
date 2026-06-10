#include "outputs.h"

// ============================================================
//  outputs.cpp — Implementare ieșiri
// ============================================================
 
//  State intern alarma
static bool          s_alarmActive      = false;
static unsigned long s_lastAlarmTick    = 0;

// State intern buton beeps
static unsigned long s_lastButtonBeep   = 0;


  
//  Inițializare
void outputs_init() {
    pinMode(LED_VERDE_PIN,    OUTPUT);
    pinMode(LED_ROSU_PIN,     OUTPUT);
    pinMode(LED_ALBASTRU_PIN, OUTPUT);
    pinMode(BUZZER_PIN,       OUTPUT);

    led_allOff();
    digitalWrite(BUZZER_PIN, LOW);

    Serial.println(F("Outputs: LED D6/D7/D4, Buzzer D5 initializate"));
}


  
//  LED-uri
void led_greenOn()  { digitalWrite(LED_VERDE_PIN,    HIGH); }
void led_greenOff() { digitalWrite(LED_VERDE_PIN,    LOW);  }
void led_redOn()    { digitalWrite(LED_ROSU_PIN,     HIGH); }
void led_redOff()   { digitalWrite(LED_ROSU_PIN,     LOW);  }
void led_blueOn()   { digitalWrite(LED_ALBASTRU_PIN, HIGH); }
void led_blueOff()  { digitalWrite(LED_ALBASTRU_PIN, LOW);  }

void led_allOff() {
    led_greenOff();
    led_redOff();
    led_blueOff();
}


  
//  Buzzer — sunete blocante (durate scurte, acceptabil)
void buzzer_confirmSound() {
    tone(BUZZER_PIN, BUZZER_CONFIRM_FREQ, BUZZER_CONFIRM_DUR);
    delay(BUZZER_CONFIRM_DUR + 20);
}

void buzzer_errorSound() {
    tone(BUZZER_PIN, BUZZER_ERROR_FREQ, BUZZER_ERROR_DUR);
    delay(BUZZER_ERROR_DUR + 20);
}


  
//  Buzzer — alarmă non-blocantă
void buzzer_alarmStart() {
    s_alarmActive   = true;
    s_lastAlarmTick = millis();
    tone(BUZZER_PIN, BUZZER_ALARM_FREQ, BUZZER_ALARM_DUR);
    Serial.println(F("Outputs: alarma PORNITA"));
}

void buzzer_alarmStop() {
    s_alarmActive = false;
    noTone(BUZZER_PIN);
    Serial.println(F("Outputs: alarma OPRITA"));
}

void buzzer_alarmTick() {
    if (!s_alarmActive) return;
    if (millis() - s_lastAlarmTick >= BUZZER_ALARM_INTERVAL) {
        s_lastAlarmTick = millis();
        tone(BUZZER_PIN, BUZZER_ALARM_FREQ, BUZZER_ALARM_DUR);
    }
}

bool buzzer_isAlarmActive() {
    return s_alarmActive;
}
  
//  Buzzer — feedback buton (non-blocant, apelat din TEMP_OPEN)
void buzzer_buttonTick(unsigned long now, unsigned long startMs) {
    // Activ doar în fereastra TIMEOUT_BUTON_MS de la startMs
    if (now - startMs >= TIMEOUT_BUTON_MS) return;

    if (now - s_lastButtonBeep >= BUZZER_BUTON_INTERVAL) {
        s_lastButtonBeep = now;
        tone(BUZZER_PIN, BUZZER_BUTON_FREQ, BUZZER_BUTON_DUR);
    }
}
