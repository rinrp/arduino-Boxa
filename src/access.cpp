#include "access.h"
#include <SPI.h>
#include <MFRC522.h>

// ============================================================================
// Definiții pini pentru modul RFID
// ============================================================================

#define RFID_SDA_PIN 10
#define RFID_RST_PIN 9
#define RFID_MOSI_PIN 11
#define RFID_MISO_PIN 12
#define RFID_SCK_PIN 13

MFRC522 mfrc522(RFID_SDA_PIN, RFID_RST_PIN);

// UID autorizat
byte uidValid[4] = {0xCA, 0xFD, 0xA1, 0x80};

// ============================================================================
// Inițializare modul RFID
// ============================================================================

void access_init() {
  SPI.begin();
  mfrc522.PCD_Init();
  Serial.println("RFID: MFRC522 inițializat");
}

// ============================================================================
// Citire și validare UID
// ============================================================================

bool access_isCardDetected() {
  return mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial();
}

byte* access_getUID() {
  return mfrc522.uid.uidByte;
}

byte access_getUIDLength() {
  return mfrc522.uid.size;
}

bool access_isUIDValid() {
  if (access_getUIDLength() != 4) {
    return false;
  }
  for (byte i = 0; i < 4; i++) {
    if (access_getUID()[i] != uidValid[i]) {
      return false;
    }
  }
  return true;
}

void access_stopCommunication() {
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

void access_printUID() {
  Serial.print("RFID: UID detectat:");
  for (byte i = 0; i < access_getUIDLength(); i++) {
    Serial.print(" ");
    if (access_getUID()[i] < 0x10) {
      Serial.print('0');
    }
    Serial.print(access_getUID()[i], HEX);
  }
  Serial.println();
}
