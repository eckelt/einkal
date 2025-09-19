# e-ink Calendar (ESP32-C3 + 5.79" 4‑Color E‑Paper)

Kompaktes Projekt zum Anzeigen des heutigen Kalenders (MS Graph / iCal verdichtet) auf einem 5.79" vier‑farb E‑Paper. Updates erfolgen komplett offline über BLE – kein WLAN erforderlich – inklusive Zeitsynchronisation.

## Features
* Tagesansicht mit Zeitachse (08–18 Uhr) und Spalten-Layout für überlappende Termine (dynamische Column-/Span-Berechnung via `CalLayout` Bibliothek).
* Automatischer Zeilenumbruch für Titel / Ort / Organisator.
* Icons für: Serien-Events (+ moved), Online-Meeting, Anhänge, Wichtigkeit.
* Abgesagte Termine: Weiß mit gelbem Rahmen statt gelbem Vollfeld.
* Deutsche Kopfzeile (Wochentag + Datum), Update-Zeit unten rechts (lokale CET/CEST Zeit).
* BLE Upload großer JSON Payloads (Chunking, MTU 247) mit robustem Buffer + Timeout.
* Zeitsynchronisation via BLE (`TIME:<epoch>`), kein NTP nötig.
* Hash-basierter Redraw: Display wird nur aktualisiert, wenn sich relevante Event-Daten geändert haben oder das Datum wechselt.
* Optional erzwungener Refresh (`LENF:` Header) für Layout-/Darstellungs-Tests ohne Datenänderung.

## Hardware
* MCU: Seeed XIAO ESP32-C3
* Display: GDEY0579F51 (5.79" 4C) – Treiber `GxEPD2_0579c_GDEY0579F51`
* Pins (aktuell im Code):
  * CS: 7
  * DC: 4
  * RST: 5
  * BUSY: 3
  * PWR: 21 (High = an)
  * SCK: 6
  * MOSI: 10

## Build & Flash (PlatformIO)
1. VS Code öffnen, Projekt laden.
2. Anpassen falls nötig: `platformio.ini` (Environment `seeed_xiao_esp32c3`).
3. Flash:
	- Menü oder Terminal: `pio run -t upload -e seeed_xiao_esp32c3`.
4. Serielle Ausgabe mit 115200 Baud überwachen.

## Dateistruktur (relevant)
```
src/main.cpp                # Firmware (BLE, Rendering, Hash, Time)
cal.py                      # Python Tool (Fetch + Condense + BLE Transfer)
data/calendar-condensed.json# (Beispiel / SPIFFS Upload) letzte Kalenderdatei
lib/CalLayout/              # Layout Algorithmus (Columns, Spanning)
```

## BLE Protokoll
Ein einzelnes Write-Characteristic (UUIDs in `main.cpp`). Zwei Befehlstypen:

1. Zeit setzen:
	`TIME:<epochSeconds>\n`
	* Epoch = UTC Sekunden (Python: `int(time.time())`).
	* Firmware setzt `settimeofday` + TZ (CET/CEST).
	* Kein Redraw alleine; nur Timestamp beim nächsten Kalender-Refresh sichtbar (optional erweiterbar).

2. Kalender übertragen:
	Header + Payload (eine oder mehrere Writes):
	* Normal: `LEN:<bytes>\n` gefolgt von Roh-JSON (UTF-8)
	* Force:  `LENF:<bytes>\n` gefolgt von Roh-JSON – erzwingt Redraw selbst bei unverändertem Hash.
	* Der erste Chunk darf bereits Payload nach dem Newline enthalten.
	* Weitere Chunks enthalten nur Payload.
	* Transfer endet nach exakt `<bytes>` empfangenen Nutzdaten (Buffer clamp). Timeout 5s Inaktivität → Reset.

## Hash-basierter Redraw
Beim Abschluss eines Transfers:
1. JSON parsen → heutige Events extrahieren.
2. Normalisierte Concats: `start|end|title|location|organizer|Flags` je Event in Reihenfolge.
3. CRC32 berechnet (Implementierung inline in `updateCalendarFromJson`).
4. Wenn: Datum unverändert UND Hash == letzter Hash UND kein `LENF:` → kein Redraw.
5. Sonst: Vollständiges Re-Rendering, neue Hash/Datum Werte in RTC RAM persistiert (`RTC_DATA_ATTR`).

## Python Tool (`cal.py`)
Funktionen:
* (Optional) Microsoft Graph Abruf + Kondensierung (falls konfiguriert – Code anpassbar für ICS).
* BLE Transfer inkl. Chunking / kombinierter Header-Payload bei kleinen Dateien.
* Zeit vorab senden (`--ble-send-time`).
* Nur Zeit senden (`--ble-time-only`).

### Wichtige Argumente (Auszug)
```
--ble                Aktiviert BLE Upload
--ble-address        Direkte Adresse (sonst Scan nach Name Prefix "CalSync")
--ble-send-time      Vor dem Kalender Epoch-Zeit senden
--ble-time-only      Nur Zeit setzen, keinen Kalender schicken
--chunk-size N       Maximale Chunk-Größe (Default dynamisch / MTU-abhängig)
--ble-force-response Erzwingt Write mit Response bei allen Chunks
--ble-chunk-delay S  Delay (Sekunden) zwischen Chunks (Große Payloads)
```

### Force Redraw vom Host
Aktuell noch nicht automatisch parametrisierbar – für Tests kann man im Skript das Header-Präfix von `LEN:` auf `LENF:` ändern (oder Option ergänzen). Geplanter Switch: `--ble-force-redraw`.

### Beispiel (nur Zeit + Kalender):
```
python3 cal.py --ble --ble-send-time
```

### Beispiel (nur Zeit setzen, kein Redraw – bisher):
```
python3 cal.py --ble --ble-send-time --ble-time-only
```

## Energie / Refresh Strategie
* Redraw nur wenn: neues Datum, Daten geändert, oder explizit `LENF:`.
* Weniger unnötige E‑Paper Updates → weniger Ghosting & Strom.
* Deep Sleep Re-Aktivierung möglich, sobald BLE-Nutzungsmuster final (Platzhalter im Code kommentiert).

## Erweiterungen (Roadmap Ideen)
* Option `--ble-force-redraw` (Python) → sendet `LENF:`.
* Teil-Refresh nur Uhrzeit nach TIME.
* Kompression (zur Zeit roh UTF‑8).
* ACK / CRC Host ↔ Gerät.
* Font-Glyph Verdickung (d/g/n) – ToDo.

## Troubleshooting
| Problem | Hinweis |
|---------|---------|
| Keine Aktualisierung nach Upload | Logs prüfen: "Unverändert (Datum & Events-Hash)" = erwartetes Verhalten |
| Zeit stimmt nicht | Sicherstellen `TIME:` gesendet wurde bevor Kalender angezeigt oder Force-Redraw auslösen |
| Transfer bricht ab | Prüfe Timeout (5s). Ggf. `--ble-chunk-delay` erhöhen |
| Hash soll ignoriert werden | Mit `LENF:` Header senden |

## Tools
* Font Generator: https://rop.nl/truetype2gfx
* Bitmap → C Array: https://javl.github.io/image2cpp

---
Stand: Hash + TIME Sync + BLE adaptiv. README zuletzt aktualisiert für Hash-basierten Redraw & LENF.