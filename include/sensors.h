#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>

// ============================================================================
// Definiții pini senzori și modem
// ============================================================================

#define MODEM_TX_PIN 2         // Modem TX
#define MODEM_RX_PIN 3         // Modem RX

#define SENZOR_MAGNETIC_PIN A0  // Pin pentru senzor magnetic KY-021
#define BUTON_DESCHIDERE_PIN A1 // Pin pentru buton deschidere

// ============================================================================
// Declarații funcții senzori
// ============================================================================

void magneticSensor_init();
void button_init();

bool magneticSensor_isDoorClosed();
bool magneticSensor_isDoorOpen();

bool button_isPressed();

void magneticSensor_printState();

#endif
