# Außenthermometer (ESP32-S3)

Funkthermometer für draußen: Ein **SHT35** misst Temperatur und relative
Luftfeuchte, ein **ESP32-S3-DevKitC-1 N16R8** stellt die Werte als Webseite und
als JSON im Heimnetz bereit. Ist kein WLAN erreichbar oder soll das Gerät neu
eingerichtet werden, öffnet es einen eigenen Access Point mit einer
Konfigurationsseite.

## Funktionen

- Messung alle 10 s über I²C, CRC-geprüft (SHT35, Single-Shot, hohe Wiederholrate)
- Statusanzeige auf der RGB-LED des Boards (WLAN, Sensor, Feldstärke)
- `GET /` – dunkle Webseite mit Temperatur und Feuchte, aktualisiert sich alle 10 s
- `GET /api/values` (auch `/api`) – dieselben Werte als JSON
- Uhrzeit per NTP inkl. Zeitzone Europa/Berlin
- WLAN-Zugangsdaten und Gerätename werden in NVS gespeichert, änderbar über die
  Konfigurationsseite – ohne Neubau der Firmware
- Im Heimnetz per Namen erreichbar: `http://<name>.local/` (mDNS), unabhängig
  vom DNS des Routers
- Konfigurations-AP mit Netzwerkscan und Formular
- Reset-Grund wird beim Start geloggt (z. B. `power on`, `BROWNOUT`,
  `task watchdog`)

## Hardware

| Bauteil | Anschluss |
|---|---|
| SHT35 VDD | 3V3 |
| SHT35 GND | GND |
| SHT35 SDA | **GPIO4** |
| SHT35 SCL | **GPIO5** |
| SHT35 ADDR | GND (Adresse 0x44) |

Externe 4,7 kΩ-Pull-ups von SDA und SCL nach 3V3 sind bestückt; die internen
Pull-ups sind zusätzlich aktiv. Der Bustakt liegt bei **50 kHz**, weil der Fühler
über eine **1,5 m lange Leitung** nach draußen geführt wird: Die Kabellänge
erhöht die Buskapazität, mit 4k7 kommt man bei 100 kHz auf etwa 0,7 µs
Flankenzeit (I²C-Grenze 1 µs) – bei 50 kHz bleibt Reserve. Eine gestörte
Übertragung wird einmal wiederholt, bevor sie als Fehlmessung zählt.

I²C über längere Strecken: ab etwa 1 m Leitung wirken sich Kapazität und
Einstreuungen aus. Bewährt haben sich verdrillte Adern mit GND, kürzere
Leitungen, kleinere Pull-ups (2k2) und ein niedrigerer Takt. Der Wert steht in
[main/board.h](main/board.h) als `BOARD_I2C_FREQ_HZ`.

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

## Statusanzeige (RGB-LED)

Die adressierbare WS2812 auf dem DevKitC-1 zeigt den Betriebszustand. Alle
Blinkmuster nutzen **125 ms an / 125 ms aus**; „N× blinken" heißt N Blinks
hintereinander, danach eine Pause.

### Grün – WLAN

| Zustand | Anzeige |
|---|---|
| Verbindung wird aufgebaut, oder kein WLAN erreichbar | dauerhaftes Blinken, 4× pro Sekunde (125 ms an/aus) |
| Konfigurationsmodus (Access Point aktiv) | 2× blinken, dann 1000 ms aus |
| Mit dem Heimnetz verbunden | leuchtet dauerhaft |

### Rot – Sensor

| Zustand | Anzeige |
|---|---|
| SHT35 beim Start nicht gefunden (I²C-Fehler) | leuchtet dauerhaft |
| Drei Messungen hintereinander fehlgeschlagen (I²C-Fehler, CRC-Fehler, Sensor abgezogen) | leuchtet dauerhaft |
| Sensor liefert wieder einen gültigen Wert | geht aus |
| Sensor in Ordnung | aus |

Ein einzelner Fehlversuch (z. B. eine gestörte Übertragung) schaltet die LED
nicht ein – erst drei Fehlversuche in Folge (bei 10 s Messabstand also nach etwa
30 s) gelten als Ausfall. Der letzte gültige Messwert bleibt in dieser Zeit auf
der Webseite sichtbar, sein Alter wächst in `age_s` mit.

### Blau – Feldstärke

Wird nur angezeigt, solange eine WLAN-Verbindung besteht.

| Empfangspegel | Anzeige |
|---|---|
| 0 bis −50 dBm (gut) | 3× blinken, dann 1000 ms aus |
| −50 bis −65 dBm (mittel) | 2× blinken, dann 1000 ms aus |
| −65 bis −80 dBm (schwach) | 1× blinken, dann 1000 ms aus |
| unter −80 dBm | aus |

Der Pegel wird einmal pro Sekunde abgefragt, die Blinkmuster laufen unabhängig
davon weiter. Die Farben werden gemischt, damit gleichzeitig sichtbar ist:
Beispiel „grün dauerhaft + blau 3× blinkend" = verbunden, gute Verbindung.

### Einstellen

In [main/board.h](main/board.h):

```c
#define BOARD_RGB_LED_GPIO              38   /* DevKitC-1 v1.1 */
#define BOARD_RGB_LED_GPIO_ALT          48   /* DevKitC-1 initiale Version */
#define BOARD_RGB_LED_BRIGHTNESS        64   /* Rot/Blau, 0..255 */
#define BOARD_RGB_LED_BRIGHTNESS_GREEN  32   /* Grün wirkt bei gleicher PWM heller */
```

Das Board existiert in zwei Revisionen, die die LED auf **GPIO38** (v1.1) bzw.
**GPIO48** (initiale Version) führen. Beide Pins werden angesteuert, der Pin
ohne LED bleibt wirkungslos – die Firmware passt damit auf beide Boards. Wer
sicher weiß, welche Revision er hat, kann den anderen Pin auf `0` setzen und
spart eine RMT-Einheit.

Treiber: Komponente `espressif/led_strip` (WS2812 über RMT), eingetragen in
[main/idf_component.yml](main/idf_component.yml) und wird beim ersten Build
nach `managed_components/` geladen. Dort steht auch `espressif/mdns` für die
Namensauflösung (siehe Abschnitt *Endpunkte*).

## Watchdog

Damit sich das Gerät nach einem Hänger selbst erholt, ist der ESP-IDF-Task-
Watchdog aktiv ([main/watchdog.c](main/watchdog.c)):

| Einstellung | Wert |
|---|---|
| Timeout | 60 s |
| Reaktion | Panic und Neustart |
| Überwachte Tasks | Sensor, Status-LED, BOOT-Taster, Netzwerkscan (Konfigmodus) |
| Idle-Tasks | nicht überwacht (lange Flash-/Löschvorgänge sollen nicht auslösen) |

Jeder dieser Tasks meldet sich in seiner Schleife an (`watchdog_subscribe()`)
und ruft pro Durchlauf `watchdog_feed()` auf. Bleibt ein Feed aus – etwa weil
der I²C-Treiber hängt oder ein Deadlock auf einer Sperre –, startet das Gerät
nach 60 s neu. Der Grund steht anschließend im Log:

```text
E (60955) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
E (60955) task_wdt:  - sensor (CPU 1)
E (60955) task_wdt: Aborting.
...
I (870) app: outdoor thermometer starting (reset: task watchdog)
```

Der Timeout ist bewusst großzügig: Der Sensor-Task schläft 10 s zwischen zwei
Messungen, der Netzwerkscan braucht ein paar Sekunden.

Nicht überwacht sind die Tasks des HTTP-Servers und der mDNS-Komponente – sie
gehören der ESP-IDF und legen keine Task-Handles offen. Ein Hänger dort würde
durch fehlende HTTP-Antworten auffallen, nicht durch einen Neustart.

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

### Aufruf über den Gerätenamen (mDNS)

Das Gerät meldet sich per mDNS an, also **ohne** den DNS des Routers zu
benötigen:

```text
http://aussenthermometer.local/
http://aussenthermometer.local/api/values
```

Der Name wird aus dem Gerätenamen gebildet ([main/netcfg.c](main/netcfg.c),
`netcfg_hostname()`): Kleinbuchstaben, Ziffern und Bindestriche; Umlaute werden
ausgeschrieben, alles andere wird zum Bindestrich, doppelte und randständige
Bindestriche entfallen:

| Eingabe | Hostname |
|---|---|
| `Außenthermometer` | `aussenthermometer` |
| `Test Gerät Küche` | `test-geraet-kueche` |
| `Garten/West 2` | `garten-west-2` |
| `---` oder leer | `thermometer` (Rückfallwert) |

Derselbe Name wird als DHCP-Hostname gesetzt, erscheint also auch in der Liste
der FRITZ!Box. Die Konfigurationsseite zeigt ihn unter dem Namensfeld an und
aktualisiert ihn beim Tippen; die Seite nach dem Speichern nennt die neue
Adresse ebenfalls. Der Dienst wird als `_http._tcp` angekündigt, damit das
Gerät auch in Netzwerk-Scannern und in der Netzwerkansicht von Windows/macOS
auftaucht. Im Konfigurationsmodus (Access Point) ist der Responder gestoppt,
weil die Adresse zum Heimnetz gehört.

Hinweis: `.local`-Namen brauchen mDNS auf dem Client. Windows 10/11, macOS,
Linux mit Avahi und Android unterstützen das; manche Geräte (ältere Android-
Versionen, Gäste-WLANs mit Client-Isolation) nicht – dort funktioniert weiter
die IP-Adresse.

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
| [main/statusled.c](main/statusled.c) | Statusanzeige auf der RGB-LED (Blinkmuster, Pegelstufen) |
| [main/watchdog.c](main/watchdog.c) | Task-Watchdog: Timeout, An-/Abmeldung, Panic bei Hänger |
| [main/webserver.c](main/webserver.c) | Endpunkte `/` und `/api/values` |
| [main/configmode.c](main/configmode.c) | Konfigurations-AP, DNS-Responder, Formular, Speichern |
| [main/wifi.c](main/wifi.c) | Station, Access Point, Netzwerkscan, Ereignisse |
| [main/netcfg.c](main/netcfg.c) | Einstellungen in NVS, Hostname |
| [main/discovery.c](main/discovery.c) | mDNS-Ankündigung `<name>.local` |
| [main/clock.c](main/clock.c) | NTP und Zeitzone |
| [main/textutil.c](main/textutil.c) | HTML-/JSON-Escaping |
| [main/board.h](main/board.h) | Pins, I²C-Adresse, RGB-LED (GPIO, Helligkeit) |
| [main/connect.example.h](main/connect.example.h) | Vorlage für die WLAN- und Geräte-Konfiguration |

## Fehlersuche

| Symptom | Ursache / Abhilfe |
|---|---|
| `no I2C device answers` im Log | Verdrahtung, 3V3/GND, SDA/SCL vertauscht. Der Start scannt den Bus und nennt gefundene Adressen. |
| `I2C devices found: 0x45 …` | ADDR liegt an VDD: `BOARD_SHT35_ADDRESS` auf `0x45` setzen. |
| Kein WLAN-AP nach BOOT-Taste | Taster zu kurz gedrückt, oder beim **Einschalten** gedrückt (dann Download-Modus). Einmal normal neu starten. |
| Konfigseite lädt nicht | `http://192.168.4.1/` vollständig eintippen (mit `http://`), nicht `https://`. |
| Gerät startet unerwartet neu | Log beim Start nennt den Grund, z. B. `BROWNOUT (supply too weak)`. Bei `task watchdog` stand vorher im Log, welcher Task nicht mehr reagiert hat. |
| `<name>.local` wird nicht gefunden | mDNS am Client fehlt/deaktiviert (Gäste-WLAN, Client-Isolation), oder der Name ist anders als erwartet – er steht im Log (`discovery: announced as ...`) und auf der Konfigurationsseite. Alternativ die IP verwenden. |
| WLAN-Reichweite bricht ein (RSSI deutlich schlechter und schwankend) | Antennenstecker sitzt nicht oder ist beschädigt: U.FL/IPEX gerade andrücken, Kabel nicht knicken. U.FL ist nur für wenige Steckzyklen ausgelegt, schräges Aufdrücken verbiegt den Mittelstift der Buchse. Bei WROOM-1U-Modulen gibt es keine PCB-Antenne – ohne intaktes Kabel bleibt nur Leckstrahlung (Verbindung bricht dann ganz weg). Bei -67 dBm blinkt die blaue LED einmal. |
| RGB-LED bleibt dunkel | Firmware passt auf GPIO38 und GPIO48; leuchtet nichts, ist die LED-Helligkeit zu niedrig (`BOARD_RGB_LED_BRIGHTNESS`) oder die Board-Revision nutzt einen anderen Pin. |
| Port belegt beim Flashen | Ein anderes Programm (Monitor, serielles Terminal) hält COM10. |
