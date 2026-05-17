#ifndef NB_IOT_H
#define NB_IOT_H

#include <Arduino.h>

// NB-IoT / Live Objects parameters
#define NB_IOT_APN "net"
#define NB_IOT_SMS_CENTRE "+40744946000"
#define NB_IOT_SMS_RECIPIENT "3523"

void initModem();
void sendCommand(const String &cmd, int delayTime);
bool connectNetwork();
bool sendData(const String &message);

#endif
