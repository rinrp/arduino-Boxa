#include <Arduino.h>
#include "sensors.h"
#include "access.h"
#include "outputs.h"
#include "nb_iot.h"

// ============================================================================
// Stări pentru State Machine   

enum StareSistem {
  IDLE,           // Stare armată, monitorizare normală
  ACCESS_GRANTED, // Acces permis prin card valid
  ACCESS_DENIED,  // Card invalid
  TEMP_OPEN,      // Deschidere temporară prin buton
  ALARM           // Alarmă activă
};

// ============================================================================
// Variabile globale pentru gestionarea stărilor

StareSistem stareCurenta = IDLE;

unsigned long timpStartDeschidere = 0;
const unsigned long delayDeschidereYala = 1000;   // 1 secundă până la aprinderea LED albastru pentru card valid
const unsigned long durataAsteptareDeschidere = 6000; // 6 secunde pentru detectarea deschiderii după LED albastru aprins
const unsigned long durataRefuzCard = 2000;       // 2 secunde LED roșu pentru card invalid
const unsigned long durataBeepsButon = 9000;     // 9 secunde beep pentru buton

unsigned long timpUltimBip = 0;
const unsigned long intervalBip = 500;
bool buttonBeepsActive = false;
unsigned long timpBlueOn = 0;

bool usaDeschisaInCiclu = false;
bool cardBlueOn = false;

bool stabilizareActiva = false;
unsigned long timpStabilizareStart = 0;
const unsigned long durataStabilizare = 2000;  // 2 secunde de filtrare la închidere
// ============================================================================
// Variabile pentru modul "Ieșire Autorizată" (buton)
// ============================================================================
bool exitAuthorized = false;   // true = alarma este inhibată pentru ieșire
bool exitDoorOpened = false;   // detectează că ușa s-a deschis după buton

unsigned long ultimulTimpAfisare = 0;
const unsigned long intervalAfisare = 1000;

// ============================================================================
// Setup

void setup() {
  Serial.begin(9600);
  while (!Serial);

  Serial.println("====================================");
  Serial.println("Sistem pornit - Control acces RFID");
  Serial.println("====================================");

  access_init();
  led_init();
  buzzer_init();
  magneticSensor_init();
  button_init();

  // Asigură LED-urile stinse la pornire
  led_blueOff();
  led_greenOff();
  led_redOff();

  initModem();
  if (connectNetwork()) {
    sendData("NB-IoT OK");
  } else {
    Serial.println(F("NB-IoT: network connect failed"));
  }

  Serial.println("Inițializare completă. Sistem în stare IDLE.");
  Serial.println();
}

// ============================================================================
// Loop

void loop() {
  if (millis() - ultimulTimpAfisare > intervalAfisare) {
    ultimulTimpAfisare = millis();
    magneticSensor_printState();
  }
  // Gestionare modul Ieșire Autorizată: rearmează când ușa se închide după deschidere
  if (exitAuthorized) {
    if (magneticSensor_isDoorOpen()) {
      exitDoorOpened = true;
    }
    if (exitDoorOpened && magneticSensor_isDoorClosed()) {
      exitAuthorized = false;
      exitDoorOpened = false;
      Serial.println("Ușă închisă după acționare buton. Sistem re-armat");
    }
  }

  if (stabilizareActiva) {
    if (millis() - timpStabilizareStart >= durataStabilizare) {
      stabilizareActiva = false;
      // La finalul perioadei de stabilizare stinge toate LED-urile (albastru + verde + roșu)
      led_blueOff();
      led_greenOff();
      led_redOff();
      Serial.println("Sistem ARMAT");
    }
  }

  switch (stareCurenta) {
    case IDLE:
      // În stare de veghe: asigură LED-urile stinse
      led_blueOff();
      led_greenOff();
      led_redOff();
      if (access_isCardDetected()) {
        access_printUID();

        if (access_isUIDValid()) {
          Serial.println("Card valid");
          led_greenOn();
          buzzer_confirmSound();
          stareCurenta = ACCESS_GRANTED;
          timpStartDeschidere = millis();
          cardBlueOn = false;
          usaDeschisaInCiclu = false;
        } else {
          Serial.println("Acces Respins");
          led_redOn();
          buzzer_errorSound();
          stareCurenta = ACCESS_DENIED;
          timpStartDeschidere = millis();
        }

        access_stopCommunication();
      }

      if (button_isPressed()) {
        Serial.println("Buton acționat: Deschiderea ușii...");
        led_blueOn();
        buttonBeepsActive = true;
        timpUltimBip = millis();
        timpStartDeschidere = millis();
        stareCurenta = TEMP_OPEN;
        usaDeschisaInCiclu = false;
      }

      if (!stabilizareActiva && magneticSensor_isDoorOpen()) {
        Serial.println("Alarmă: Ușă deschisă fortat");
        led_redOn();
        buzzer_alarmStart();
        stareCurenta = ALARM;
      }
      break;

    case ACCESS_GRANTED:
      led_greenOn();

      if (!cardBlueOn && millis() - timpStartDeschidere >= delayDeschidereYala) {
        led_blueOn();
        cardBlueOn = true;
        timpBlueOn = millis();
        usaDeschisaInCiclu = false;
        Serial.println("Yală activată - LED albastru aprins");
      }

      if (cardBlueOn) {
        if (magneticSensor_isDoorOpen()) {
          usaDeschisaInCiclu = true;
        }

        if (usaDeschisaInCiclu && magneticSensor_isDoorClosed()) {
          Serial.println("Ușă închisă. Se activează perioada de stabilizare de 2 secunde...");
          led_blueOff();
          stabilizareActiva = true;
          timpStabilizareStart = millis();
          stareCurenta = IDLE;
          cardBlueOn = false;
          usaDeschisaInCiclu = false;
          break;
        }

        if (!usaDeschisaInCiclu && millis() - timpBlueOn >= durataAsteptareDeschidere) {
          Serial.println("Timp expirat: Ușa nu a fost deschisă. Re-armare automată");
          led_blueOff();
          led_greenOff();
          stareCurenta = IDLE;
          cardBlueOn = false;
          usaDeschisaInCiclu = false;
        }
      }
      break;

    case ACCESS_DENIED:
      led_redOn();

      if (millis() - timpStartDeschidere >= durataRefuzCard) {
        led_redOff();
        Serial.println("Sistem re-armat automat - Card invalid finalizat");
        stareCurenta = IDLE;
      }

      if (magneticSensor_isDoorOpen()) {
        Serial.println("Alarmă: Ușă deschisă fortat");
        buzzer_alarmStart();
        stareCurenta = ALARM;
      }
      break;

    case TEMP_OPEN:
      led_blueOn();

      if (buttonBeepsActive) {
        if (millis() - timpStartDeschidere < durataBeepsButon) {
          if (millis() - timpUltimBip >= intervalBip) {
            tone(BUZZER_PIN, 800, 80);
            timpUltimBip = millis();
          }
        } else {
          buttonBeepsActive = false;
          noTone(BUZZER_PIN);
          led_blueOff();
          Serial.println("Sistem re-armat automat - Ușă neutilizată");
          stareCurenta = IDLE;
        }
      }

      if (magneticSensor_isDoorOpen()) {
        usaDeschisaInCiclu = true;
      }
      if (usaDeschisaInCiclu && magneticSensor_isDoorClosed()) {
        Serial.println("Ușă închisă - Sistem rearmată imediat");
        buttonBeepsActive = false;
        noTone(BUZZER_PIN);
        led_blueOff();
        stareCurenta = IDLE;
        usaDeschisaInCiclu = false;
      }
      break;

    case ALARM:
      led_redOn();
      buzzer_alarmManage();

      if (magneticSensor_isDoorClosed()) {
        Serial.println("Ușă închisă - Sistem asigurat");
        buzzer_alarmStop();
        led_redOff();
        stareCurenta = IDLE;
      }
      break;
  }
}
