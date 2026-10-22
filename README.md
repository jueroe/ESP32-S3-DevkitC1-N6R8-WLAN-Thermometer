# Außenthermometer (ESP32-S3)

Funkthermometer für draußen: Ein **SHT35** misst Temperatur und relative
Luftfeuchte, ein **ESP32-S3-DevKitC-1 N16R8** stellt die Werte als Webseite und
als JSON im Heimnetz bereit. Ist kein WLAN erreichbar oder soll das Gerät neu
eingerichtet werden, öffnet es einen eigenen Access Point mit einer
Konfigurationsseite.

## Funktionen

- Messung alle 10 s über I²C, CRC-geprüft (SHT35, Single-Shot, hohe Wiederholrate)
- `GET /` – dunkle Webseite mit Temperatur und Feuchte, aktualisiert sich alle 10 s
- `GET /api/values` (auch `/api`) – dieselben Werte als JSON
- Uhrzeit per NTP inkl. Zeitzone Europa/Berlin
- WLAN-Zugangsdaten und Gerätename werden in NVS gespeichert, änderbar über die
  Konfigurationsseite – ohne Neubau der Firmware
- Konfigurations-AP mit Netzwerkscan und Formular
- Reset-Grund wird beim Start geloggt (z. B. `power on`, `BROWNOUT`)

## Hardware

| Bauteil | Anschluss |
|---|---|
| SHT35 VDD | 3V3 |
| SHT35 GND | GND |
| SHT35 SDA | **GPIO4** |
| SHT35 SCL | **GPIO5** |
| SHT35 ADDR | GND (Adresse 0x44) |

Externe 4,7 kΩ-Pull-ups von SDA und SCL nach 3V3 sind bestückt; die internen
Pull-ups sind zusätzlich aktiv. Der Bustakt liegt bei 100 kHz.

GPIO4/GPIO5 liegen auf der linken Kopfleiste (J1), also der Reihe mit 3V3 oben
und GND unten. Nicht verwenden: GPIO0 (BOOT), 3/46 (Strapping), 19/20 (USB),
43/44 (Konsole), 26–32 (Flash), 33–37 (Octal-PSRAM).

Pins und Adresse stehen in [main/board.h](main/board.h):

```c
#define BOARD_I2C_SDA_GPIO   4
#define BOARD_I2C_SCL_GPIO   5
#define BOARD_SHT35_ADDRESS  0x44   /* 0x45, wenn ADDR an VDD liegt */
```

## Bauen und flashen

Voraussetzungen: ESP-IDF **v6.1** (getestet) und ein USB-Kabel am
USB-UART-Anschluss des DevKitC-1 (im Beispiel COM10).

```powershell
Copy-Item main\connect.example.h main\connect.h   # einmalig, dann Werte eintragen
idf.py set-target esp32s3
idf.py build
idf.py -p COM10 flash monitor
```

In VS Code: ESP-IDF-Erweiterung, Setup `C:\esp\v6.1\esp-idf`, Port COM10,
Ziel `esp32s3` (siehe [.vscode/settings.json](.vscode/settings.json)).

Board-Einstellungen stehen in [sdkconfig.defaults](sdkconfig.defaults)
(16 MB Flash, 8 MB Octal-PSRAM, 240 MHz) und
[partitions.csv](partitions.csv) (4 MB Applikation, keine OTA-Slots).

## WLAN einstellen

`main/connect.h` enthält nur die **Vorgaben für den ersten Start** und ist in
[.gitignore](.gitignore) eingetragen, damit das WLAN-Passwort nicht im
Repository landet. Vorlage: [main/connect.example.h](main/connect.example.h).

Sobald auf der Konfigurationsseite gespeichert wurde, gelten die Werte aus dem
NVS-Speicher. Ein Wechsel des Netzes braucht dann keinen Neubau.

## Konfigurationsmodus

Startet automatisch, wenn das gespeicherte WLAN nicht gefunden wird, oder auf
Wunsch:

> **BOOT im laufenden Betrieb 3 Sekunden gedrückt halten.**

Dann:

1. Am Handy/PC mit `thermometer-xxxx` verbinden (Passwort `thermometer`,
   Name und Passwort stehen in `connect.h`; `xxxx` sind die letzten beiden
   MAC-Bytes, damit mehrere Geräte unterscheidbar sind).
2. `http://192.168.4.1/` im Browser öffnen.
3. SSID aus der Liste wählen (oder eintippen), Passwort und Gerätename
   eintragen, „Speichern und neu starten".
4. Das Gerät startet neu und meldet sich im Heimnetz an.

Wichtig:

- **BOOT beim Einschalten gedrückt halten funktioniert nicht** – GPIO0 ist der
  Download-Strap, der Chip bleibt dann im ROM-Download-Modus und die
  Anwendung startet gar nicht.
- Das Konfigurations-WLAN hat **kein Internet**. Das Handy zeigt deshalb ein
  „!" am WLAN-Symbol; die Seite `192.168.4.1` ist trotzdem erreichbar. Eine
  automatische „Anmelden im Netzwerk"-Meldung erscheint nicht zuverlässig
  (Android prüft zusätzlich per HTTPS gegen Google), ist aber auch nicht nötig.
- Ein leeres Passwortfeld bei unveränderter SSID behält das gespeicherte
  Passwort; bei neuem Netz bedeutet es „offenes Netz".

## Endpunkte

| Pfad | Inhalt |
|---|---|
| `/` | Webseite (HTML, Aktualisierung alle 10 s) |
| `/api`, `/api/values` | JSON (siehe unten) |
| `/save` (nur im Konfigmodus) | Formular speichern, Gerät startet neu |

Beispiel `curl http://192.168.123.123/api/values`:

```json
{
  "name": "Außenthermometer",
  "valid": true,
  "temperature_c": 23.8,
  "humidity_rh": 46.9,
  "age_s": 1,
  "time": "22.10. 19:25",
  "ip": "192.168.123.123",
  "rssi_dbm": -46,
  "uptime_s": 11
}
```

- `valid` ist `false`, solange noch kein Messwert vorliegt (dann sind
  `temperature_c` und `humidity_rh` `null`).
- `age_s` ist das Alter der letzten Messung in Sekunden, `time` die lokale
  Uhrzeit (vor dem NTP-Abgleich `01.01. 00:00`).
- JSON nutzt den Dezimalpunkt, die HTML-Seite das Dezimalkomma.

## Aufbau des Codes

| Datei | Aufgabe |
|---|---|
| [main/main.c](main/main.c) | Startablauf, Sensor-Task, NTP-Task, BOOT-Taster (3 s) |
| [main/sensor.c](main/sensor.c) | SHT35-Treiber (I²C-Master, CRC-8), letzter Messwert im Cache |
| [main/webserver.c](main/webserver.c) | Endpunkte `/` und `/api/values` |
| [main/configmode.c](main/configmode.c) | Konfigurations-AP, DNS-Responder, Formular, Speichern |
| [main/wifi.c](main/wifi.c) | Station, Access Point, Netzwerkscan, Ereignisse |
| [main/netcfg.c](main/netcfg.c) | Einstellungen in NVS, Hostname |
| [main/clock.c](main/clock.c) | NTP und Zeitzone |
| [main/textutil.c](main/textutil.c) | HTML-/JSON-Escaping |
| [main/board.h](main/board.h) | Pins und I²C-Adresse |
| [main/connect.example.h](main/connect.example.h) | Vorlage für die WLAN- und Geräte-Konfiguration |

## Fehlersuche

| Symptom | Ursache / Abhilfe |
|---|---|
| `no I2C device answers` im Log | Verdrahtung, 3V3/GND, SDA/SCL vertauscht. Der Start scannt den Bus und nennt gefundene Adressen. |
| `I2C devices found: 0x45 …` | ADDR liegt an VDD: `BOARD_SHT35_ADDRESS` auf `0x45` setzen. |
| Kein WLAN-AP nach BOOT-Taste | Taster zu kurz gedrückt, oder beim **Einschalten** gedrückt (dann Download-Modus). Einmal normal neu starten. |
| Konfigseite lädt nicht | `http://192.168.4.1/` vollständig eintippen (mit `http://`), nicht `https://`. |
| Gerät startet unerwartet neu | Log beim Start nennt den Grund, z. B. `BROWNOUT (supply too weak)`. |
| Port belegt beim Flashen | Ein anderes Programm (Monitor, serielles Terminal) hält COM10. |
