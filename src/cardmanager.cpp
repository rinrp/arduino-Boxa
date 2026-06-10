#include "cardmanager.h"
#include "config.h"
#include <EEPROM.h>
#include <ctype.h>

#define EEPROM_COUNT_ADDR 0
#define EEPROM_DATA_ADDR  1

static byte s_count = 0;

static int uidAddr(byte index) {
    return EEPROM_DATA_ADDR + (int)index * CARD_UID_LENGTH;
}

static bool uidEqualsEEPROM(byte index, const byte* uid) {
    int addr = uidAddr(index);
    for (byte b = 0; b < CARD_UID_LENGTH; b++) {
        if (EEPROM.read(addr + b) != uid[b]) return false;
    }
    return true;
}

static void writeUID(byte index, const byte* uid) {
    int addr = uidAddr(index);
    for (byte b = 0; b < CARD_UID_LENGTH; b++) {
        EEPROM.update(addr + b, uid[b]);
    }
}

static bool isEEPROMBlank() {
    if (EEPROM.read(EEPROM_COUNT_ADDR) != 0xFF) return false;
    for (byte b = 0; b < CARD_UID_LENGTH; b++) {
        if (EEPROM.read(EEPROM_DATA_ADDR + b) != 0xFF) return false;
    }
    return true;
}

void cardmanager_init() {
    s_count = EEPROM.read(EEPROM_COUNT_ADDR);

    if (s_count == 0xFF || s_count > CARD_MAX_COUNT || isEEPROMBlank()) {
        s_count = 1;
        EEPROM.update(EEPROM_COUNT_ADDR, s_count);
        writeUID(0, UID_VALID);
        Serial.println(F("CardManager: primul boot - card default adaugat"));
        cardmanager_printAll();
        return;
    }

    Serial.print(F("CardManager: "));
    Serial.print(s_count);
    Serial.println(F(" carduri incarcate din EEPROM"));
    cardmanager_printAll();
}

bool cardmanager_isValid(const byte* uid) {
    if (!uid) return false;
    for (byte i = 0; i < s_count; i++) {
        if (uidEqualsEEPROM(i, uid)) return true;
    }
    return false;
}

bool cardmanager_add(const byte* uid) {
    if (!uid) return false;

    if (cardmanager_isValid(uid)) {
        Serial.println(F("CardManager: card deja existent"));
        return false;
    }

    if (s_count >= CARD_MAX_COUNT) {
        Serial.println(F("CardManager: lista plina"));
        return false;
    }

    writeUID(s_count, uid);
    s_count++;
    EEPROM.update(EEPROM_COUNT_ADDR, s_count);

    Serial.print(F("CardManager: card adaugat, total="));
    Serial.println(s_count);
    return true;
}

bool cardmanager_remove(const byte* uid) {
    if (!uid) return false;

    for (byte i = 0; i < s_count; i++) {
        if (uidEqualsEEPROM(i, uid)) {
            byte last[CARD_UID_LENGTH];
            int lastAddr = uidAddr(s_count - 1);

            for (byte b = 0; b < CARD_UID_LENGTH; b++) {
                last[b] = EEPROM.read(lastAddr + b);
            }

            writeUID(i, last);
            s_count--;
            EEPROM.update(EEPROM_COUNT_ADDR, s_count);

            Serial.print(F("CardManager: card eliminat, total="));
            Serial.println(s_count);
            return true;
        }
    }

    Serial.println(F("CardManager: card negasit"));
    return false;
}

byte cardmanager_count() {
    return s_count;
}

void cardmanager_printAll() {
    if (s_count == 0) {
        Serial.println(F("CardManager: niciun card autorizat"));
        return;
    }

    for (byte i = 0; i < s_count; i++) {
        Serial.print(F("  Card "));
        Serial.print(i);
        Serial.print(F(": "));

        int addr = uidAddr(i);
        for (byte b = 0; b < CARD_UID_LENGTH; b++) {
            byte v = EEPROM.read(addr + b);
            if (v < 0x10) Serial.print('0');
            Serial.print(v, HEX);
            if (b < CARD_UID_LENGTH - 1) Serial.print(':');
        }
        Serial.println();
    }
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool cardmanager_parseUID(const char* str, byte* uidOut) {
    if (!str || !uidOut) return false;

    byte idx = 0;
    const char* p = str;

    while (*p && idx < CARD_UID_LENGTH) {
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) return false;

        int h = hexVal(p[0]);
        int l = hexVal(p[1]);
        if (h < 0 || l < 0) return false;

        uidOut[idx++] = (byte)((h << 4) | l);
        p += 2;

        if (*p == ':') p++;
    }

    return idx == CARD_UID_LENGTH && *p == '\0';
}
