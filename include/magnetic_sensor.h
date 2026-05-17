#ifndef MAGNETIC_SENSOR_H
#define MAGNETIC_SENSOR_H

#include <Arduino.h>

// ============================================================================
// Definiții Pini Senzor Magnetic
// ============================================================================

#define SENZOR_MAGNETIC_PIN 2  // Pin pentru senzor magnetic reed KY-021

// ============================================================================
// Declarații Funcții Senzor Magnetic
// ============================================================================

/**
 * Inițializează pinul senzorului magnetic ca INPUT_PULLUP.
 * Trebuie apelată din setup().
 * 
 * Notă:
 * - HIGH: Ușă închisă (câmp magnetic prezent)
 * - LOW: Ușă deschisă (câmp magnetic absent)
 */
void magneticSensor_init();

/**
 * Verifică dacă ușa este închisă.
 * Returnează true dacă senzorul detectează câmp magnetic (ușă închisă).
 */
bool magneticSensor_isDoorClosed();

/**
 * Verifică dacă ușa este deschisă.
 * Returnează true dacă senzorul nu detectează câmp magnetic (ușă deschisă).
 */
bool magneticSensor_isDoorOpen();

/**
 * Afișează starea ușii în Serial Monitor.
 */
void magneticSensor_printState();

#endif
