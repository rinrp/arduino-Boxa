# Handover proiect: arduino-Boxa

## 1. Scop proiect
Acest proiect este un sistem de control acces RFID bazat pe Arduino Uno, cu interfață Bluetooth NB-IoT pentru raportare și comenzi de la distanță prin Orange Live Objects.

## 2. Arhitectură generală
Codul este împărțit pe module principale:
- `src/main.cpp` – bucla principală, stările sistemului și coordonarea modulelor.
- `src/access.cpp` / `include/access.h` – citirea cardului RFID MFRC522.
- `src/sensors.cpp` / `include/sensors.h` – senzor reed pentru ușă și buton de deschidere manuală.
- `src/outputs.cpp` / `include/outputs.h` – LED-uri și buzzer.
- `src/cardmanager.cpp` / `include/cardmanager.h` – stocare și gestionare carduri în EEPROM.
- `src/nbiot.cpp` / `include/nbiot.h` – comunicație NB-IoT, MQTT Live Objects și comenzi remote.
- `include/config.h` – definiții hardware, timpi de funcționare și configurații generale.
- `platformio.ini` – configurare PlatformIO, board Arduino Uno și variabile de build pentru Live Objects.

## 3. Hardware actualmente folosit
- Arduino Uno
- Module RFID MFRC522: SDA = D10, RST = D9, SPI = D11/D12/D13
- Reed switch ușă: A0
- Buton deschidere manuală: A1
- LED verde: D6
- LED roșu: D7
- LED albastru: D4
- Buzzer pasiv: D5
- Modem NB-IoT (BC92): TX = D2, RX = D3

## 4. Funcționalități implementate
### 4.1 Acces RFID și statemachine
- Detectare card RFID și citire UID.
- Pe card valid:
  - stare `ACCESS_GRANTED`
  - LED verde pornit
  - buzzer de confirmare
  - publicare eveniment `access_granted` prin NB-IoT
  - așteptare deschidere ușă și stabilizare la închidere
- Pe card invalid:
  - stare `ACCESS_DENIED`
  - LED roșu pornit
  - buzzer eroare
  - publicare eveniment `access_denied`
- În starea `IDLE` se poate apăsa butonul fizic pentru deschidere temporară (`TEMP_OPEN`).
- Dacă ușa se deschide fără autorizație, se trece în `ALARM` și se pornește alarmă.
- Există mecanism de stabilizare pentru a evita rearmarea imediată după închidere.

### 4.2 Senzori și intrări
- `door_isClosed()` / `door_isOpen()` bazate pe reed switch cu `INPUT_PULLUP`.
- Buton manual cu debouncing.
- Log periodic la serial pentru schimbarea stării ușii.

### 4.3 Ieșiri și feedback
- LED verde = acces permis
- LED roșu = acces refuzat / alarmă
- LED albastru = yală activă / deschidere manuală
- Buzzer:
  - bip confirmare acces
  - bip eroare acces
  - alarmă intermitentă non-blocantă
  - beep de feedback în starea `TEMP_OPEN`

### 4.4 Gestiune carduri persistente
- Modul `cardmanager` stochează cardurile autorizate în EEPROM.
- Layout EEPROM:
  - adresa 0 = număr carduri
  - adresele următoare = UID-uri 4-bytes
- Capacitate pentru maxim 10 carduri.
- La prima pornire, dacă EEPROM-ul nu este inițializat, se adaugă implicit cardul `CA:FD:A1:80`.
- Sunt suportate:
  - verificare card valid
  - adăugare card nou
  - ștergere card
  - listare carduri în serial

### 4.5 Conectivitate NB-IoT și Live Objects
- Modul `nbiot` implementează conectarea secvențială la modem prin AT commands.
- Inițializarea rulează non-blocant în `nbiot_initTick()`.
- Se folosesc comenzi AT pentru:
  - test AT
  - dezactivare echo
  - verificare înregistrare NB-IoT (`AT+CEREG?`)
  - configurare MQTT (`AT+QMTCFG`)
  - deschidere conexiune TCP și autentificare MQTT
  - subscriere topic MQTT
- Publică evenimente JSON pe `dev/data` (Live Objects publish topic).
- Primi comenzi de pe `dev/cmd` și le translatează în evenimente interne:
  - `{ "cmd":"open" }` → `CMD_OPEN` → deschidere la distanță
  - `{ "cmd":"card_add","uid":"XX:XX:XX:XX" }` → `CMD_CARD_ADD` → adaugă card în EEPROM
  - `{ "cmd":"card_remove","uid":"XX:XX:XX:XX" }` → `CMD_CARD_REMOVE` → șterge card din EEPROM
  - `{ "cmd":"status" }` → `CMD_STATUS` → publică starea curentă

## 5. Configurare actuală
- `platformio.ini` definește variabile de build pentru Live Objects:
  - `LO_API_KEY`
  - `LO_DEVICE_ID`
  - `LO_SMS_ADMIN_NR`
  - `LO_MQTT_HOST`
  - `LO_MQTT_PORT`
  - `LO_STREAM_ID`
  - `LO_TOPIC_PUB`
  - `LO_TOPIC_SUB`
- `include/nbiot.h` conține valori implicite de Live Objects, dar acestea trebuie înlocuite cu datele reale pentru producție.
- Atenție: datele de autentificare ar trebui securizate și să nu fie păstrate în codul sursă public.

## 6. Observații și limitări curente
- NB-IoT este implementat cu AT commands și funcționează în fundal; însă starea `INIT_FAILED` dezactivează raportarea cloud și SMS.
- Modulul SMS există ca fallback, dar transmiterea se declanșează doar pentru evenimente critice în codul NB-IoT.
- Controlul acces bazat pe carduri valide este migrat la EEPROM, dar încă există un UID implicit definit în `config.h`.
- Nu există o interfață de configurare locală pentru adăugarea/ștergerea cardurilor - doar comenzi cloud sau EEPROM direct.

## 7. Ce se poate construi următor
1. Adăugare meniu local / serial pentru administrare carduri (listare, adăugare, ștergere).
2. Debug și testare completă NB-IoT în acoperire reală Orange Live Objects.
3. Asigurarea securității datelor și a parametrilor Live Objects.
4. Extindere stare `ALARM` cu reset fizic sau recuperare automată mai robustă.
5. Implementare actuală pentru starea `ACCESS_GRANTED`: deschiderea fizică a yălii prin releu/încărcare.
6. Adăugare suport pentru multiple carduri fără UID hardcodat (`access_isUIDValid` ar putea fi eliminat sau adaptat).

## 8. Recomandări imediate
- Verifică și actualizează `LO_API_KEY` și `LO_DEVICE_ID` reale în `platformio.ini` sau în `include/nbiot.h`.
- Testează comanda remote `open` și comanda `status` prin Live Objects.
- Confirmă comportamentul de stabilizare la închidere și timpii din `config.h`.
- Dacă treci la producție, mută datele sensibile în variabile de mediu sau o soluție de configurare externă.

---

Acest document oferă o imagine de ansamblu asupra funcționalităților și arhitecturii proiectului la stadiul actual. Poți folosi această bază pentru a planifica următoarele dezvoltări și teste.
