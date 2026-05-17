#include "sensors.h"

// ============================================================================
// Variabile globale pentru buton
// ============================================================================

unsigned long ultimulTimpApasareButon = 0;
const unsigned long debounceDelay = 200;  // 200 ms debouncing
static bool stareButonAnterioara = HIGH;

// ============================================================================
// Inițializare senzori
// ============================================================================

void magneticSensor_init() {
  pinMode(SENZOR_MAGNETIC_PIN, INPUT_PULLUP);
  Serial.println("Senzor Magnetic: Inițializat");
}

void button_init() {
  pinMode(BUTON_DESCHIDERE_PIN, INPUT_PULLUP);
  Serial.println("Senzor Buton: Inițializat");
}

// ============================================================================
// Citire senzor magnetic
// ============================================================================

bool magneticSensor_isDoorClosed() {
  return digitalRead(SENZOR_MAGNETIC_PIN) == LOW;
}

bool magneticSensor_isDoorOpen() {
  return digitalRead(SENZOR_MAGNETIC_PIN) == HIGH;
}

void magneticSensor_printState() {
  if (magneticSensor_isDoorClosed()) {
    Serial.println("Senzor Magnetic: Ușă Închisă");
  } else {
    Serial.println("Senzor Magnetic: Ușă Deschisă");
  }
}

// ============================================================================
// Citire buton
// ============================================================================

bool button_isPressed() {
  bool stareButonCurenta = digitalRead(BUTON_DESCHIDERE_PIN);

  if (stareButonCurenta == LOW && stareButonAnterioara == HIGH &&
      (millis() - ultimulTimpApasareButon) > debounceDelay) {
    ultimulTimpApasareButon = millis();
    stareButonAnterioara = stareButonCurenta;
    Serial.println("Senzor Buton: Apăsat");
    return true;
  }

  stareButonAnterioara = stareButonCurenta;
  return false;
}
