#include "cardmanager.h"
#include "config.h"
#include <EEPROM.h>

// ============================================================
//  cardmanager.cpp — Implementare gestiune carduri RFID
// ============================================================

// Layout EEPROM:
//   Adresa EEPROM_BASE_ADDR:          număr carduri (byte)
//   Adresa EEPROM_BASE_ADDR + 1 + i*4: UID-ul i (4 bytes)

#define EEPROM_BASE_ADDR   0
#define EEPROM_COUNT_ADDR  EEPROM_BASE_ADDR
#define EEPROM_DATA_ADDR  (EEPROM_BASE_ADDR + 1)

// Cache în RAM pentru acces rapid
static byte s_cards[CARD_MAX_COUNT][CARD_UID_LENGTH];
static byte s_count = 0;

// ============================================================
//  Helpers interni
// ============================================================

static void saveToEEPROM() {
    EEPROM.update(EEPROM_COUNT_ADDR, s_count);
    for (byte i = 0; i < s_count; i++) {
        int addr = EEPROM_DATA_ADDR + i * CARD_UID_LENGTH;
        for (byte b = 0; b < CARD_UID_LENGTH; b++) {
            EEPROM.update(addr + b, s_cards[i][b]);
        }
    }
}

static bool uidEquals(const byte* a, const byte* b) {
    for (byte i = 0; i < CARD_UID_LENGTH; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// ============================================================
//  Inițializare
// ============================================================

static bool isEEPROMBlank() {
    for (byte b = 0; b < CARD_UID_LENGTH; b++) {
        if (EEPROM.read(EEPROM_DATA_ADDR + b) != 0xFF) {
            return false;
        }
    }
    return true;
}

void cardmanager_init() {
    s_count = EEPROM.read(EEPROM_COUNT_ADDR);

    // Validare: dacă EEPROM-ul e neiniţializat (0xFF) sau corupt
    if (s_count == 0xFF || s_count > CARD_MAX_COUNT ||
        (s_count == 0 && isEEPROMBlank())) {
        s_count = 1;
        for (byte b = 0; b < CARD_UID_LENGTH; b++) {
            s_cards[0][b] = UID_VALID[b];
        }
        saveToEEPROM();
        Serial.println(F("CardManager: primul boot - card CA:FD:A1:80 adaugat"));
        return;
    }

    // Încarcă cardurile în cache RAM
    for (byte i = 0; i < s_count; i++) {
        int addr = EEPROM_DATA_ADDR + i * CARD_UID_LENGTH;
        for (byte b = 0; b < CARD_UID_LENGTH; b++) {
            s_cards[i][b] = EEPROM.read(addr + b);
        }
    }

    Serial.print(F("CardManager: "));
    Serial.print(s_count);
    Serial.println(F(" carduri incarcate din EEPROM"));
    cardmanager_printAll();
}

// ============================================================
//  Verificare
// ============================================================

bool cardmanager_isValid(const byte* uid) {
    for (byte i = 0; i < s_count; i++) {
        if (uidEquals(s_cards[i], uid)) return true;
    }
    return false;
}

// ============================================================
//  Adăugare
// ============================================================

bool cardmanager_add(const byte* uid) {
    // Verifică dacă există deja
    if (cardmanager_isValid(uid)) {
        Serial.println(F("CardManager: card deja existent, ignorat"));
        return false;
    }
    // Verifică capacitate
    if (s_count >= CARD_MAX_COUNT) {
        Serial.println(F("CardManager: lista plina!"));
        return false;
    }
    // Adaugă în cache și salvează
    for (byte b = 0; b < CARD_UID_LENGTH; b++) {
        s_cards[s_count][b] = uid[b];
    }
    s_count++;
    saveToEEPROM();

    Serial.print(F("CardManager: card adaugat, total="));
    Serial.println(s_count);
    return true;
}

// ============================================================
//  Ștergere
// ============================================================

bool cardmanager_remove(const byte* uid) {
    for (byte i = 0; i < s_count; i++) {
        if (uidEquals(s_cards[i], uid)) {
            // Mută ultimul card pe poziția i (swap cu ultimul)
            for (byte b = 0; b < CARD_UID_LENGTH; b++) {
                s_cards[i][b] = s_cards[s_count - 1][b];
            }
            s_count--;
            saveToEEPROM();

            Serial.print(F("CardManager: card eliminat, total="));
            Serial.println(s_count);
            return true;
        }
    }
    Serial.println(F("CardManager: card negasit pentru eliminare"));
    return false;
}

// ============================================================
//  Utilitare
// ============================================================

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
        for (byte b = 0; b < CARD_UID_LENGTH; b++) {
            if (s_cards[i][b] < 0x10) Serial.print('0');
            Serial.print(s_cards[i][b], HEX);
            if (b < CARD_UID_LENGTH - 1) Serial.print(':');
        }
        Serial.println();
    }
}

bool cardmanager_parseUID(const char* str, byte* uidOut) {
    // Format așteptat: "CA:FD:A1:80" (cu sau fără două puncte)
    byte idx = 0;
    const char* p = str;

    while (*p && idx < CARD_UID_LENGTH) {
        // Citește un byte hex (2 caractere)
        char hi = *p++;
        char lo = *p++;

        auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };

        int h = hexVal(hi);
        int l = hexVal(lo);
        if (h < 0 || l < 0) return false;

        uidOut[idx++] = (byte)((h << 4) | l);

        // Sare peste separator ':' dacă există
        if (*p == ':') p++;
    }

    return (idx == CARD_UID_LENGTH);
}