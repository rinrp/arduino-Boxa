#ifndef CARDMANAGER_H
#define CARDMANAGER_H

#include <Arduino.h>

#define CARD_UID_LENGTH   4
#define CARD_MAX_COUNT    10   // maxim 10 carduri stocate

void cardmanager_init();

bool cardmanager_isValid(const byte* uid);
bool cardmanager_add(const byte* uid);
bool cardmanager_remove(const byte* uid);

byte cardmanager_count();
void cardmanager_printAll();

bool cardmanager_parseUID(const char* str, byte* uidOut);

#endif // CARDMANAGER_H
