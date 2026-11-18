# ESP32 LIN Sniffer

Ein PlatformIO-Projekt für einen ESP32-WROOM, das einen LIN-Bus mit 19.200 Baud beobachtet. Es erkennt LIN-Frames, meldet alle zwei Sekunden den Busstatus und – für LIN-ID `0x04` – die letzten zwei Datenbytes. Zusätzlich misst es die Batteriespannung und kann die Messwerte auf einer SD-Karte protokollieren.

## Prototyp

![Aufbau des ESP32-LIN-Sniffer-Prototyps](assets/lin-sniffer-prototype.jpeg)

## Funktionen

- Empfang und Auswertung von LIN-Frames (Break, Sync, PID und Daten).
- Erkennung, ob LIN-Verkehr vorhanden ist und ob ein Frame mit der Ziel-ID `0x04` empfangen wurde.
- Zuordnung der beiden ersten Datenbytes der Ziel-ID zu bekannten Lichtzuständen.
- Batteriespannungsmessung über den ADC, gemittelt aus acht Messungen.
- Ausgabe des Status über die serielle Schnittstelle und optionales Logging im Zwei-Sekunden-Takt auf SD-Karte.

## Hardware und Anschlüsse

| ESP32-Pin | Anschluss | Hinweise |
| --- | --- | --- |
| GPIO 4 | LIN-Empfang | An den RXD-Ausgang eines geeigneten LIN-Transceivers anschließen. |
| GPIO 34 | Batteriespannung | ADC-Eingang hinter dem Spannungsteiler. |
| GPIO 5 | SD-Karte CS | Chip-Select für die SD-Karte. |
| GPIO 18 | SD-Karte SCK | VSPI-Takt (Standardbelegung). |
| GPIO 19 | SD-Karte MISO | VSPI-Daten vom SD-Modul. |
| GPIO 23 | SD-Karte MOSI | VSPI-Daten zum SD-Modul. |

LIN ist ein Fahrzeugbus mit Spannungen und elektrischen Störungen, die einen ESP32 beschädigen können. Den LIN-Bus **nicht direkt** mit einem ESP32-Pin verbinden. Ein geeigneter LIN-Transceiver muss den Buspegel in ein ESP32-kompatibles RX-Signal umsetzen; ESP32 und Transceiver benötigen eine gemeinsame Masse.

Der Spannungsteiler für die Batteriespannung ist laut Schaltungskommentar als `VIN — 22 kΩ — GPIO 34 — 5,6 kΩ — GND` vorgesehen. Die Software verwendet für die Berechnung die Werte 21,7 kΩ und 5,5 kΩ. Die ADC-Spannung muss innerhalb des zulässigen ESP32-Eingangsbereichs bleiben. Vor dem Anschluss die maximale Eingangsspannung und die tatsächlichen Widerstandswerte prüfen.

Ein SD-Modul sollte mit dem ESP32 kompatibel sein und an die VSPI-Pins sowie an GPIO 5 als CS angeschlossen werden.

## Bauen und Flashen

Voraussetzung ist [PlatformIO](https://platformio.org/) mit installierter ESP32-Plattform. Die Projektumgebung `upesy_wroom` ist in `platformio.ini` konfiguriert.

```sh
pio run
pio run --target upload
pio device monitor
```

Der serielle Monitor verwendet `115200` Baud. Der LIN-Empfang ist auf `19200` Baud eingestellt.

## Ausgabe und SD-Logging

Nach dem Start zeigt die serielle Schnittstelle die Konfiguration. Anschließend wird alle zwei Sekunden eine Statuszeile ausgegeben, zum Beispiel mit LIN- und Lichtstatus, den letzten erkannten Datenbytes und der Batteriespannung. Die angezeigten Lichtzustände werden anhand einer im Programm hinterlegten Byte-Tabelle interpretiert; nicht bekannte Kombinationen erscheinen als `Unbekannt`.

Wenn eine SD-Karte initialisiert wurde, erstellt das Programm die erste freie Datei von `/lin_0001.txt` bis `/lin_9999.txt`. Vorhandene Dateien werden nicht überschrieben. Jede Datei beginnt mit der Kopfzeile:

```text
Nr LIN Licht Data0 Data1 Interpr Batt_V
```

Die folgenden Zeilen sind durch Leerzeichen getrennt. `LIN` und `Licht` sind `1` (erkannt) oder `0` (nicht erkannt); `Data0`, `Data1` und `Interpr` enthalten die letzten Daten der Ziel-ID. Wenn keine passenden Daten im Zeitfenster empfangen wurden, stehen dort `--`. `Batt_V` enthält die Batteriespannung in Volt mit zwei Nachkommastellen. Bei fehlgeschlagener SD-Initialisierung läuft die serielle Statusausgabe weiter, das SD-Logging ist dann deaktiviert.

## Projektstruktur

- `src/main.cpp` – LIN-Empfang, Auswertung, Batteriespannungsmessung und Logging.
- `platformio.ini` – PlatformIO-Konfiguration.
- `include/`, `lib/`, `test/` – vorgesehene PlatformIO-Verzeichnisse für Header, private Bibliotheken und Tests.
