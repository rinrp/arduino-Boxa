#include "access.h"
#include <SPI.h>
#include <MFRC522.h>

// ============================================================
//  access.cpp — Implementare RFID MFRC522 (Versiune Curată)
// ============================================================

static MFRC522 s_mfrc522(RFID_SDA_PIN, RFID_RST_PIN);

//  Inițializare
void access_init() {
    SPI.begin();
    s_mfrc522.PCD_Init();
    Serial.println(F("RFID: MFRC522 initializat (SDA=D10, RST=D9)"));
}

//  Citire card
bool access_isCardDetected() {
    return s_mfrc522.PICC_IsNewCardPresent() &&
           s_mfrc522.PICC_ReadCardSerial();
}

byte* access_getUID() {
    return s_mfrc522.uid.uidByte;
}

byte access_getUIDLength() {
    return s_mfrc522.uid.size;
}

// Funcția corectată (folosește direct UID_VALID din config.h)
bool access_isUIDValid() {
    if (access_getUIDLength() != UID_VALID_LENGTH) return false;

    const byte* uid = access_getUID();
    for (byte i = 0; i < UID_VALID_LENGTH; i++) {
        if (uid[i] != UID_VALID[i]) return false;
    }
    return true;
}

void access_stopCommunication() {
    s_mfrc522.PICC_HaltA();
    s_mfrc522.PCD_StopCrypto1();
}

void access_uidToString(char* out, size_t outSize) {
    byte len = access_getUIDLength();
    byte* uid = access_getUID();
    size_t pos = 0;

    for (byte i = 0; i < len && pos + 3 < outSize; i++) {
        if (i > 0) out[pos++] = ':';
        byte hi = (uid[i] >> 4) & 0x0F;
        byte lo =  uid[i]       & 0x0F;
        out[pos++] = hi < 10 ? '0' + hi : 'A' + hi - 10;
        out[pos++] = lo < 10 ? '0' + lo : 'A' + lo - 10;
    }
    out[pos] = '\0';
}

void access_printUID() {
    char buf[32];
    access_uidToString(buf, sizeof(buf));
    Serial.print(F("RFID: UID="));
    Serial.println(buf);
}