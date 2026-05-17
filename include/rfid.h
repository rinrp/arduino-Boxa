#ifndef RFID_H
#define RFID_H

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>

// ============================================================================
// Definiții Pini RFID
// ============================================================================

#define RFID_SDA_PIN 10  // Pin SDA pentru MFRC522
#define RFID_RST_PIN 9   // Pin RST pentru MFRC522

// ============================================================================
// Declarații Funcții RFID
// ============================================================================

/**
 * Inițializează modulul RFID MFRC522.
 * Trebuie apelată din setup() după SPI.begin().
 */
void rfid_init();

/**
 * Verifică dacă un card RFID nou este prezent și citește seria acestuia.
 * Returnează true dacă un card a fost detectat și citit.
 */
bool rfid_isCardDetected();

/**
 * Obține UID-ul cardului detectat.
 * Returnează pointer la array de 4 octeți cu UID-ul.
 */
byte* rfid_getUID();

/**
 * Obține lungimea UID-ului detectat.
 */
byte rfid_getUIDLength();

/**
 * Verifică dacă UID-ul detectat este valid.
 * UID-ul valid este predefinit: A1 B2 C3 D4
 * Returnează true dacă UID-ul este valid.
 */
bool rfid_isUIDValid();

/**
 * Oprește comunicația cu cardul RFID.
 * Trebuie apelată după citirea unui card.
 */
void rfid_stopCommunication();

/**
 * Afișează UID-ul detectat în Serial Monitor.
 */
void rfid_printUID();

#endif
