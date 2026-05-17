#ifndef ACCESS_H
#define ACCESS_H

#include <Arduino.h>

// ============================================================================
// Declarații funcții RFID și validarea UID
// ============================================================================

void access_init();
bool access_isCardDetected();
byte* access_getUID();
byte access_getUIDLength();
bool access_isUIDValid();
void access_stopCommunication();
void access_printUID();

#endif
