#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include "config.h"

// ============================================================
//  sensors.h — Reed switch KY-021 și buton cu debouncing
//
//  Pini: REED_PIN=A0, BUTON_PIN=A1 (din config.h)
// ============================================================


 
//  Inițializare
 

/** Inițializează pinii senzorului reed și butonului ca INPUT_PULLUP. */
void sensors_init();


 
//  Reed switch (senzor magnetic ușă)
 

/**
 * Returnează true dacă ușa este închisă.
 * (câmp magnetic prezent → reed contact închis → LOW)
 */
bool door_isClosed();

/**
 * Returnează true dacă ușa este deschisă.
 * (câmp magnetic absent → reed contact deschis → HIGH)
 */
bool door_isOpen();

/** Afișează starea ușii în Serial Monitor. */
void door_printState();


 
//  Buton deschidere manuală
 

/**
 * Returnează true o singură dată la apăsarea butonului.
 * Include debouncing de BUTON_DEBOUNCE_MS (din config.h).
 * Apelați în fiecare iterație de loop().
 */
bool button_wasPressed();

#endif // SENSORS_H
