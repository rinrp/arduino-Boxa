#include "diagnostics.h"
#include "outputs.h"
#include "sensors.h"

static void printPinMapping() {
    Serial.println(F("=== Diagnostic hardware ==="));
    Serial.println(F("Pini definiți în include/config.h:"));

    Serial.print(F("  MODEM_TX_PIN  = "));
    Serial.println(MODEM_TX_PIN);
    Serial.print(F("  MODEM_RX_PIN  = "));
    Serial.println(MODEM_RX_PIN);

    Serial.print(F("  LED_ALBASTRU_PIN = "));
    Serial.println(LED_ALBASTRU_PIN);
    Serial.print(F("  BUZZER_PIN       = "));
    Serial.println(BUZZER_PIN);
    Serial.print(F("  LED_VERDE_PIN    = "));
    Serial.println(LED_VERDE_PIN);
    Serial.print(F("  LED_ROSU_PIN     = "));
    Serial.println(LED_ROSU_PIN);

    Serial.print(F("  RFID_RST_PIN = "));
    Serial.println(RFID_RST_PIN);
    Serial.print(F("  RFID_SDA_PIN = "));
    Serial.println(RFID_SDA_PIN);
    Serial.println(F("  RFID MOSI = 11, MISO = 12, SCK = 13 (SPI hardware)"));

    Serial.print(F("  REED_PIN  = "));
    Serial.println(REED_PIN);
    Serial.print(F("  BUTON_PIN = "));
    Serial.println(BUTON_PIN);

    Serial.println();
}

static void testOutputs() {
    Serial.println(F("Test LED-uri și buzzer:"));

    Serial.print(F("  LED verde... "));
    led_greenOn();
    delay(250);
    led_greenOff();
    Serial.println(F("OK"));

    Serial.print(F("  LED roșu... "));
    led_redOn();
    delay(250);
    led_redOff();
    Serial.println(F("OK"));

    Serial.print(F("  LED albastru... "));
    led_blueOn();
    delay(250);
    led_blueOff();
    Serial.println(F("OK"));

    Serial.print(F("  Buzzer... "));
    tone(BUZZER_PIN, BUZZER_CONFIRM_FREQ, BUZZER_CONFIRM_DUR);
    delay(BUZZER_CONFIRM_DUR + 50);
    noTone(BUZZER_PIN);
    Serial.println(F("OK"));

    led_allOff();
    Serial.println();
}

static void testInputs() {
    Serial.println(F("Test intrări: reed și buton:"));

    int reedValue = digitalRead(REED_PIN);
    Serial.print(F("  REED_PIN (A0) raw = "));
    Serial.print(reedValue);
    Serial.print(F(" -> "));
    Serial.println(reedValue == DOOR_CLOSED_STATE ? F("ÎNCHISĂ") : F("DESCHISĂ"));

    int buttonValue = digitalRead(BUTON_PIN);
    Serial.print(F("  BUTON_PIN (A1) raw = "));
    Serial.print(buttonValue);
    Serial.print(F(" -> "));
    Serial.println(buttonValue == BUTON_APASAT ? F("APĂSAT") : F("NEAPĂSAT"));

    Serial.println(F("  Observație: butonul este configurat cu INPUT_PULLUP, deci starea normală este NEAPĂSAT (HIGH)."));
    Serial.println(F("  Apasă butonul pentru a vedea schimbarea stării în Serial Monitor."));
    Serial.println();
}

void diagnostics_runHardwareCheck() {
    printPinMapping();
    testOutputs();
    testInputs();
    Serial.println(F("=== Final diagnostic hardware ==="));
    Serial.println();
}
