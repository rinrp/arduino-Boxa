#ifndef ACCESS_H
#define ACCESS_H

#include <Arduino.h>
#include "config.h"

// ============================================================
//  access.h — Modul RFID MFRC522
//
//  Pini hardware (din config.h):
//    RFID_SDA_PIN = D10  (SS/CS)
//    RFID_RST_PIN = D9
//    MOSI = D11, MISO = D12, SCK = D13  (SPI hardware, auto)
//
//  UID autorizat: configurat în config.h
// ============================================================


  
//  Inițializare
//  Apelați după SPI.begin() — sau lăsați access_init() să
//  apeleze SPI.begin() intern (recomandat).
  

void access_init();


  
//  Citire card
  

/**
 * Verifică dacă un card nou este prezent și citește UID-ul.
 * Returnează true dacă un card a fost detectat și citit cu succes.
 * Apelați în loop() — nu blochează dacă nu e card prezent.
 */
bool access_isCardDetected();

/**
 * Obține UID-ul cardului curent detectat.
 * Valid doar imediat după access_isCardDetected() == true.
 */
byte*  access_getUID();
byte   access_getUIDLength();

/**
 * Verifică dacă UID-ul detectat corespunde cardului autorizat
 * (definit prin UID_VALID_BYTE_x în config.h).
 */
bool access_isUIDValid();

/**
 * Oprește comunicația cu cardul curent.
 * Apelați întotdeauna după procesarea unui card detectat.
 */
void access_stopCommunication();

/**
 * Construiește un string hex din UID-ul curent (ex: "CA:FD:A1:80").
 * Utile pentru logging și viitoare transmisii.
 * Buffer-ul out trebuie să aibă cel puțin 3*uidLength bytes.
 */
void access_uidToString(char* out, size_t outSize);

/** Afișează UID-ul curent în Serial Monitor. */
void access_printUID();

#endif // ACCESS_H
