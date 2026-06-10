#ifndef ACCESS_H
#define ACCESS_H

#include <Arduino.h>
#include "config.h"

void access_init();
bool access_isCardDetected();

byte*  access_getUID();
byte   access_getUIDLength();

bool access_isUIDValid();

void access_stopCommunication();
void access_uidToString(char* out, size_t outSize);
void access_printUID();

#endif // ACCESS_H
