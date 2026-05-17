#ifndef BUZZER_H
#define BUZZER_H

#include <Arduino.h>

// ============================================================================
// Definiții Pini Buzzer
// ============================================================================

#define BUZZER_PIN 8  // Pin pentru buzzer pasiv KY-006

// ============================================================================
// Declarații Funcții Buzzer
// ============================================================================

/**
 * Inițializează pinul buzzer ca OUTPUT.
 * Trebuie apelată din setup().
 */
void buzzer_init();

/**
 * Emite un sunet scurt de confirmare (1000 Hz, 200 ms).
 */
void buzzer_confirmSound();

/**
 * Emite un sunet de eroare (500 Hz, 1000 ms).
 */
void buzzer_errorSound();

/**
 * Pornește alarma intermitentă.
 * Sunet la 800 Hz, 250 ms, intermitent la 500 ms.
 */
void buzzer_alarmStart();

/**
 * Oprește alarma.
 */
void buzzer_alarmStop();

/**
 * Gestionează alarma intermitentă.
 * Trebuie apelată în loop() pentru a menține sunetul.
 */
void buzzer_alarmManage();

/**
 * Verifica dacă alarma este activă.
 */
bool buzzer_isAlarmActive();

#endif
