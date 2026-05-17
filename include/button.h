#ifndef BUTTON_H
#define BUTTON_H

#include <Arduino.h>

// ============================================================================
// Definiții Pini Buton
// ============================================================================

#define BUTON_DESCHIDERE_PIN 5  // Pin pentru buton deschidere manuală

// ============================================================================
// Declarații Funcții Buton
// ============================================================================

/**
 * Inițializează pinul butonului ca INPUT_PULLUP.
 * Trebuie apelată din setup().
 */
void button_init();

/**
 * Verifică dacă butonul a fost apăsat.
 * Include debouncing automat (200 ms).
 * Returnează true dacă butonul a fost tocmai apăsat.
 */
bool button_isPressed();

/**
 * Afișează o notificare în Serial Monitor când butonul este apăsat.
 */
void button_printPressNotification();

#endif
