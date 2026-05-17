#include "nb_iot.h"
#include "sensors.h"
#include <SoftwareSerial.h>

static SoftwareSerial nbSerial(MODEM_RX_PIN, MODEM_TX_PIN);

void sendCommand(const String &cmd, int delayTime) {
  Serial.print(F(">> "));
  Serial.println(cmd);
  nbSerial.println(cmd);
  delay(delayTime);
  while (nbSerial.available()) {
    Serial.write(nbSerial.read());
  }
}

void initModem() {
  Serial.println(F("NB-IoT: init modem"));
  nbSerial.begin(9600);
  delay(1000);
  sendCommand("AT", 1000);
  sendCommand("ATE0", 1000);
}

bool connectNetwork() {
  Serial.println(F("NB-IoT: testing connection"));
  sendCommand("AT", 1000);
  sendCommand("AT+CPIN?", 1000);
  sendCommand("AT+CEREG?", 1000);
  sendCommand("AT+CSQ", 1000);
  sendCommand("AT+CGDCONT=1,\"IP\",\"" NB_IOT_APN "\"", 1000);
  sendCommand("AT+CGATT=1", 1000);
  sendCommand("AT+CGPADDR", 1000);
  return true;
}

bool sendData(const String &message) {
  Serial.println(F("NB-IoT: sending test message via SMS"));
  sendCommand("AT+CMGF=1", 1000);
  sendCommand(String("AT+CSCA=\"" NB_IOT_SMS_CENTRE "\""), 1000);
  sendCommand(String("AT+CMGS=\"" NB_IOT_SMS_RECIPIENT "\""), 1000);
  delay(3000);
  nbSerial.print(message);
  delay(500);
  nbSerial.write(26); // CTRL+Z
  delay(5000);
  while (nbSerial.available()) {
    Serial.write(nbSerial.read());
  }
  Serial.println(F("SMS TRIMIS"));
  return true;
}
