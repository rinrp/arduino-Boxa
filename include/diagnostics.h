#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include <Arduino.h>
#include "config.h"

/**
 * Rulează un test hardware pentru a verifica definițiile pinilor și funcțiile de I/O.
 * Afișează rezultatele pe Serial Monitor.
 */
void diagnostics_runHardwareCheck();

#endif // DIAGNOSTICS_H
