#include "outputs.h"

// ============================================================================
// Variabile buzzer pentru alarmă
// ============================================================================

unsigned long timpUltimSunetAlarma = 0;
const unsigned long intervalAlarma = 500;
bool alarmActive = false;

// ============================================================================
// Helper intern
// ============================================================================

static void setLed(uint8_t pin, bool on) {
  digitalWrite(pin, on ? HIGH : LOW);
}

void beep(int durata, int repetari, int frecventa) {
  for (int i = 0; i < repetari; i++) {
    tone(BUZZER_PIN, frecventa, durata);
    unsigned long start = millis();
    while (millis() - start < (unsigned long)durata + 40) {
      // așteaptă scurt între beeps
    }
  }
}

// ============================================================================
// Inițializare LED și buzzer
// ============================================================================

void led_init() {
  pinMode(LED_VERDE_PIN, OUTPUT);
  pinMode(LED_ROSU_PIN, OUTPUT);
  pinMode(LED_ALBASTRU_PIN, OUTPUT);
  led_allOff();
  Serial.println("Outputs: LED inițializate");
}

void buzzer_init() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  Serial.println("Outputs: Buzzer inițializat");
}

// ============================================================================
// Control LED
// ============================================================================

void led_greenOn() {
  setLed(LED_VERDE_PIN, true);
}

void led_greenOff() {
  setLed(LED_VERDE_PIN, false);
}

void led_redOn() {
  setLed(LED_ROSU_PIN, true);
}

void led_redOff() {
  setLed(LED_ROSU_PIN, false);
}

void led_blueOn() {
  setLed(LED_ALBASTRU_PIN, true);
}

void led_blueOff() {
  setLed(LED_ALBASTRU_PIN, false);
}

void led_allOff() {
  led_greenOff();
  led_redOff();
  led_blueOff();
}

// ============================================================================
// Control Buzzer
// ============================================================================

void buzzer_confirmSound() {
  beep(200, 1, 1000);
}

void buzzer_errorSound() {
  beep(1200, 1, 300);
}

void buzzer_alarmStart() {
  alarmActive = true;
  timpUltimSunetAlarma = millis();
  Serial.println("Outputs: Alarmă sonoră activată");
}

void buzzer_alarmStop() {
  alarmActive = false;
  noTone(BUZZER_PIN);
  Serial.println("Outputs: Alarmă sonoră oprită");
}

void buzzer_alarmManage() {
  if (alarmActive && millis() - timpUltimSunetAlarma >= intervalAlarma) {
    timpUltimSunetAlarma = millis();
    tone(BUZZER_PIN, 800, 250);
  }
}

bool buzzer_isAlarmActive() {
  return alarmActive;
}
