#ifndef OUTPUTS_H
#define OUTPUTS_H

#include <Arduino.h>
#include "config.h"


//  Inițializare

/** Inițializează toți pinii de ieșire și stinge LED-urile. */
void outputs_init();

//  LED-uri

void led_greenOn();
void led_greenOff();

void led_redOn();
void led_redOff();

void led_blueOn();
void led_blueOff();

void led_allOff();

 
//  Buzzer
/**
 * Bip scurt de confirmare acces (1000 Hz, 200 ms).
 * Blocker — ocupă ~200ms din execuție.
 */
void buzzer_confirmSound();

/**
 * Sunet de eroare / acces refuzat (300 Hz, 1200 ms).
 * Blocker — ocupă ~1200ms.
 */
void buzzer_errorSound();

/**
 * Pornește alarma intermitentă (non-blocker).
 * Apelați o singură dată la intrarea în starea ALARM.
 */
void buzzer_alarmStart();

/** Oprește alarma. */
void buzzer_alarmStop();

/**
 * Menține ritmul alarmei intermitente.
 * Apelați în fiecare iterație de loop() cât timp alarma este activă.
 * Non-blocker — folosește millis() intern.
 */
void buzzer_alarmTick();

/** Returnează true dacă alarma este în curs. */
bool buzzer_isAlarmActive();

/**
 * Emite un bip scurt pentru feedback buton.
 * Non-blocker — folosit din starea TEMP_OPEN cu timer extern.
 */
void buzzer_buttonTick(unsigned long now, unsigned long startMs);

#endif // OUTPUTS_H
