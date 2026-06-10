#ifndef CONFIG_H
#define CONFIG_H

//  NU modifica pinii în altă parte decât aici.

//  Pini hardware (conform configurației fizice)
#define MODEM_TX_PIN        2
#define MODEM_RX_PIN        3

// Ieșiri
#define LED_ALBASTRU_PIN    4   // LED albastru — semnalizare yală / buton
#define BUZZER_PIN          5   // Buzzer pasiv KY-006
#define LED_VERDE_PIN       6   // LED verde — acces permis
#define LED_ROSU_PIN        7   // LED roșu — acces refuzat / alarmă

// RFID MFRC522 (SPI hardware)
#define RFID_RST_PIN        9
#define RFID_SDA_PIN        10
// MOSI=11, MISO=12, SCK=13 — gestionate automat de SPI.h

// Senzori intrare
#define REED_PIN            A0  // Senzor magnetic reed KY-021
#define BUTON_PIN           A1  // Buton deschidere manuală
  
//  Logică senzor reed (reed KY-021 cu INPUT_PULLUP)
//  KY-021 cu magnet prezent (ușă închisă): contact închis → LOW
//  KY-021 fără magnet (ușă deschisă):    contact deschis → HIGH
#define DOOR_CLOSED_STATE   LOW
#define DOOR_OPEN_STATE     HIGH
  
//  Logică buton (INPUT_PULLUP, activ LOW)
#define BUTON_APASAT        LOW
#define BUTON_DEBOUNCE_MS   200UL

//  UID card autorizat (4 bytes, ex: CA:FD:A1:80)
#define UID_VALID_LENGTH    4
const byte UID_VALID[UID_VALID_LENGTH] = {0xCA, 0xFD, 0xA1, 0x80};
  
//  Timing state machine (milisecunde)
// ACCESS_GRANTED: delay între LED verde și activarea yălii (LED albastru)
#define DELAY_YALA_MS           1000UL
// ACCESS_GRANTED: cât timp așteptăm ușa să se deschidă după yală activă, Dacă ușa nu se deschide în acest interval → re-armare automată
#define TIMEOUT_ASTEPTARE_USA_MS  6000UL
// ACCESS_DENIED: cât timp rămâne aprins LED-ul roșu pentru card invalid
#define DURATA_REFUZ_CARD_MS    2000UL
// TEMP_OPEN: cât timp sunt active beep-urile butonului,La expirare, dacă ușa nu s-a deschis → re-armare automată
#define TIMEOUT_BUTON_MS        9000UL
// Perioada de stabilizare după închiderea ușii, Previne re-armarea imediată și eventualele bounce-uri ale senzorului
#define DURATA_STABILIZARE_MS   2000UL
// Timeout de siguranță pentru stabilizare (forțat reset dacă blochează)
#define STABILIZARE_MAX_MS      (DURATA_STABILIZARE_MS * 30UL)

//  Buzzer — frecvențe și durate
#define BUZZER_CONFIRM_FREQ     1000    // Hz — bip scurt de confirmare
#define BUZZER_CONFIRM_DUR      150     // ms

#define BUZZER_ERROR_FREQ       300     // Hz — sunet jos, eroare
#define BUZZER_ERROR_DUR        800    // ms

#define BUZZER_ALARM_FREQ       800     // Hz — alarmă intermitentă
#define BUZZER_ALARM_DUR        250     // ms
#define BUZZER_ALARM_INTERVAL   500UL   // ms între bipuri de alarmă

#define BUZZER_BUTON_FREQ       800     // Hz — bip periodic buton
#define BUZZER_BUTON_DUR        80      // ms
#define BUZZER_BUTON_INTERVAL   500UL   // ms între bipuri buton

//  Serial debug
#define SERIAL_BAUD             9600
#define LOG_INTERVAL_MS         1000UL  // interval afișare stare reed
#define RUN_HARDWARE_DIAGNOSTIC 1

#endif // CONFIG_H
