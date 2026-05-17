#ifndef LED_H
#define LED_H

#include <Arduino.h>

// ============================================================================
// Definiții Pini LED
// ============================================================================

#define LED_VERDE_PIN 6      // LED verde pentru succes
#define LED_ROSU_PIN 7       // LED roșu pentru alarmă
#define LED_ALBASTRU_PIN 3   // LED albastru pentru status ușă/yală

// ============================================================================
// Declarații Funcții LED
// ============================================================================

/**
 * Inițializează toți pini LED ca OUTPUT.
 * Trebuie apelată din setup().
 */
void led_init();

/**
 * Aprinde LED-ul verde.
 */
void led_greenOn();

/**
 * Stinge LED-ul verde.
 */
void led_greenOff();

/**
 * Aprinde LED-ul roșu.
 */
void led_redOn();

/**
 * Stinge LED-ul roșu.
 */
void led_redOff();

/**
 * Aprinde LED-ul albastru.
 */
void led_blueOn();

/**
 * Stinge LED-ul albastru.
 */
void led_blueOff();

/**
 * Stinge toate LED-urile.
 */
void led_allOff();

/**
 * Aprinde un semnal de succes: LED verde pentru 1 secundă.
 */
void led_successSignal();

#endif
