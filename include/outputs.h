#ifndef OUTPUTS_H
#define OUTPUTS_H

#include <Arduino.h>

// ============================================================================
// Definiții pini pentru LED și buzzer
// ============================================================================

#define LED_VERDE_PIN 6      // LED verde
#define LED_ROSU_PIN 7       // LED roșu
#define LED_ALBASTRU_PIN 4   // LED albastru (simulare yală)
#define BUZZER_PIN 5         // Buzzer KY-006

// ============================================================================
// Declarații funcții de ieșire
// ============================================================================

void led_init();
void led_greenOn();
void led_greenOff();
void led_redOn();
void led_redOff();
void led_blueOn();
void led_blueOff();
void led_allOff();

void buzzer_init();
void beep(int durata, int repetari, int frecventa = 1000);
void buzzer_confirmSound();
void buzzer_errorSound();
void buzzer_alarmStart();
void buzzer_alarmStop();
void buzzer_alarmManage();
bool buzzer_isAlarmActive();

#endif
