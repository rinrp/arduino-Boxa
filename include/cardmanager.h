#ifndef CARDMANAGER_H
#define CARDMANAGER_H

#include <Arduino.h>

// ============================================================
//  cardmanager.h — Gestiune carduri RFID în EEPROM
//
//  Înlocuiește UID-ul hardcodat din config.h cu o listă
//  persistentă stocată în EEPROM.
//
//  Layout EEPROM:
//    Adresa 0:       număr carduri stocate (byte)
//    Adresa 1..N*4:  UID-uri (câte 4 bytes fiecare)
//
//  Capacitate maximă: CARD_MAX_COUNT carduri (definit mai jos)
// ============================================================

#define CARD_UID_LENGTH   4
#define CARD_MAX_COUNT    10   // maxim 10 carduri stocate

// ============================================================
//  Inițializare
// ============================================================

/** Inițializează EEPROM-ul și încarcă lista de carduri. */
void cardmanager_init();

// ============================================================
//  Verificare
// ============================================================

/**
 * Verifică dacă un UID de 4 bytes este autorizat.
 * Returnează true dacă UID-ul există în lista stocată.
 */
bool cardmanager_isValid(const byte* uid);

// ============================================================
//  Adăugare / Ștergere
// ============================================================

/**
 * Adaugă un UID nou în lista de carduri autorizate.
 * Returnează true dacă a fost adăugat cu succes.
 * Returnează false dacă lista e plină sau cardul există deja.
 */
bool cardmanager_add(const byte* uid);

/**
 * Elimină un UID din lista de carduri autorizate.
 * Returnează true dacă a fost găsit și eliminat.
 * Returnează false dacă UID-ul nu există în listă.
 */
bool cardmanager_remove(const byte* uid);

// ============================================================
//  Utilitare
// ============================================================

/** Returnează numărul de carduri stocate în prezent. */
byte cardmanager_count();

/** Afișează în Serial Monitor toate cardurile stocate. */
void cardmanager_printAll();

/**
 * Parsează un string hex de tip "CA:FD:A1:80" în 4 bytes.
 * Returnează true dacă parsearea a reușit.
 * Folosit pentru comenzile primite din cloud.
 */
bool cardmanager_parseUID(const char* str, byte* uidOut);

#endif // CARDMANAGER_H
