#include <esp_sleep.h>
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <GxEPD2_4C.h>
#include <epd4c/GxEPD2_0579c_GDEY0579F51.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold7pt7b.h>
#include <Fonts/FreeSans7pt7b.h>
#include <Fonts/FreeSans6pt7b.h>
#include <Fonts/Font5x7Fixed.h>
#include <Fonts/Font4x5Fixed.h>
#include <Icons.h>
#include <time.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <CalLayout.h> // local library in lib/CalLayout/
#include <NimBLEDevice.h>  // BLE hinzu
#include <NimBLEUtils.h>

// Optional Debug für Hash-Bildung aktivieren (1 = an, 0 = aus)
#define CAL_HASH_DEBUG 1

// ==== BLE UUIDs (beliebig, nur konsistent bleiben) ====
static const char* BLE_SERVICE_UUID       = "7e20c560-55dd-4c7a-9c61-8f6ea7d7c301";
static const char* BLE_CHARACTERISTIC_UUID = "9c5a5dd9-3c40-4e58-9d0a-95bf7cb9d302";

// Buffer für eingehende Kalenderdaten
static String bleIncoming; // legacy (will phase out)
static size_t bleExpectedLen = 0;
static bool bleForceOnFinish = false; // wird durch speziellen Header (LENF:) gesetzt
static bool   bleTransferActive = false;
static unsigned long bleLastChunkMillis = 0;
static const uint32_t BLE_TRANSFER_TIMEOUT_MS = 5000;
static char* bleBuffer = nullptr;
static size_t bleBufferWritePos = 0;
static const size_t BLE_MAX_PAYLOAD = 60000; // sanity limit to avoid huge allocations

// Vorwärtsdeklaration
bool updateCalendarFromJson(const String& jsonStr, bool forceRefresh);
void saveCalendarFile(const String& jsonStr) {
  File f = SPIFFS.open("/calendar-condensed.json", "w");
  if (!f) { Serial.println("Kalender-Datei speichern fehlgeschlagen!"); return; }
  f.print(jsonStr);
  f.close();
  Serial.println("Kalender-Datei gespeichert (/calendar-condensed.json).");
}

// BLE Callback
class CalendarCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) override {
    std::string v = chr->getValue();
    if (v.empty()) return;

    // Neuer Transfer erwartet ersten Chunk mit "LEN:<zahl>\n"
    // Sonderkommando: TIME:<epochSeconds>\n  -> setzt Systemzeit (UTC) und kehrt zurück
    if (!bleTransferActive && v.rfind("TIME:", 0) == 0) {
      size_t nl = v.find('\n');
      if (nl == std::string::npos) {
        Serial.println("TIME Header ohne Newline – ignoriert.");
        return;
      }
      std::string num = v.substr(5, nl-5);
      long long epoch = atoll(num.c_str());
      if (epoch > 100000) {
        struct timeval tv;
        tv.tv_sec = (time_t)epoch;
        tv.tv_usec = 0;
        if (settimeofday(&tv, nullptr) == 0) {
          Serial.printf("Zeit per BLE gesetzt (UTC Epoch): %lld\n", epoch);
          // Sicherstellen, dass Zeitzone gesetzt ist (falls WiFi/NTP übersprungen wurde)
          setenv("TZ","CET-1CEST,M3.5.0,M10.5.0/3",1); tzset();
        } else {
          Serial.println("settimeofday fehlgeschlagen");
        }
      } else {
        Serial.println("TIME Wert ungueltig");
      }
      return; // kein Kalendertransfer starten
    }

    if (!bleTransferActive) {
      if (v.rfind("LENF:", 0) == 0 || v.rfind("LEN:", 0) == 0) {
        size_t nlPos = v.find('\n');
        if (nlPos == std::string::npos) {
          Serial.println("LEN Header ohne Newline – Chunk verworfen.");
          return;
        }
        std::string headerStd = v.substr(0, nlPos); // z.B. "LEN:1234" oder "LENF:1234"
        if (headerStd.size() < 5) {
          Serial.println("LEN Header zu kurz – verworfen.");
          return;
        }
        bool force = (headerStd.rfind("LENF:", 0) == 0);
        bleForceOnFinish = force; // merken
        long declared = strtol(headerStd.c_str() + (force?5:4), nullptr, 10);
        if (declared <= 0) {
          Serial.println("LEN Wert ungültig (<=0) – verworfen.");
          return;
        }
        if (declared > (long)BLE_MAX_PAYLOAD) {
          Serial.printf("LEN %ld überschreitet Limit (%u) – verworfen.\n", declared, (unsigned)BLE_MAX_PAYLOAD);
          return;
        }
        bleExpectedLen = (size_t)declared;
        // Allocate / reallocate buffer
        if (bleBuffer) { free(bleBuffer); bleBuffer = nullptr; }
        bleBuffer = (char*)malloc(bleExpectedLen + 1);
        if (!bleBuffer) {
          Serial.println("Malloc fehlgeschlagen – Abbruch.");
          return;
        }
        bleBufferWritePos = 0;
        size_t restOffset = nlPos + 1; // nach dem '\n'
        if (restOffset < v.size()) {
          std::string rest = v.substr(restOffset);
          size_t restLen = rest.size();
          if (restLen) {
            if (restLen > bleExpectedLen) restLen = bleExpectedLen; // clamp
            memcpy(bleBuffer, rest.data(), restLen);
            bleBufferWritePos = restLen;
          }
          Serial.printf("(Header Chunk enthielt bereits %u Payload-Bytes)\n", (unsigned)restLen);
        } else {
          Serial.println("(Header Chunk ohne sofortige Payload)");
        }
        bleTransferActive = true;
        bleLastChunkMillis = millis();
        Serial.printf("BLE Transfer gestartet. Erwartete Länge: %u  (force=%s)\n", (unsigned)bleExpectedLen, bleForceOnFinish?"ja":"nein");
      } else {
        Serial.println("Erster Chunk ohne LEN:-Header – ignoriert.");
        return;
      }
    } else {
      // Fortsetzungs-Chunks direkt in Buffer kopieren
      size_t addLen = v.size();
      if (bleBufferWritePos + addLen > bleExpectedLen) {
        addLen = bleExpectedLen - bleBufferWritePos; // clamp overflow
      }
      if (addLen) memcpy(bleBuffer + bleBufferWritePos, v.data(), addLen);
      bleBufferWritePos += addLen;
      bleLastChunkMillis = millis();
    }

    // Fortschritt / Abschluss prüfen
    if (bleTransferActive) {
      size_t have = bleBufferWritePos;
      Serial.printf("BLE Fortschritt: %u / %u (%.1f%%)\n", (unsigned)have, (unsigned)bleExpectedLen, (have * 100.0f) / bleExpectedLen);
      if (have >= bleExpectedLen) {
        Serial.println("BLE Transfer komplett. Prüfe / speichere JSON...");
        bleTransferActive = false;
        if (bleBuffer) bleBuffer[bleExpectedLen] = '\0';
        String jsonStr = String(bleBuffer ? bleBuffer : "");
        saveCalendarFile(jsonStr);
        if (!updateCalendarFromJson(jsonStr, bleForceOnFinish)) {
          Serial.println("JSON Update fehlgeschlagen oder übersprungen.");
        }
        bleExpectedLen = 0;
        if (bleBuffer) { free(bleBuffer); bleBuffer = nullptr; }
        bleBufferWritePos = 0;
        bleForceOnFinish = false;
      }
    }
  }
};

class RestartAdvServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    Serial.println("BLE verbunden");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    Serial.println("BLE getrennt. Starte Advertising neu...");
    NimBLEDevice::startAdvertising();
  }
};

void initBLE() {
  NimBLEDevice::init("CalSync");
  // Sendeleistung setzen (abhängig von Core / NimBLE Version).
  // Manche Versionen kennen nur ESP_PWR_LVL_P9 / P6 / P3 / N0 etc.
#if defined(ESP_PWR_LVL_P9)
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); // höchste verfügbare Stufe
#elif defined(ESP_PWR_LVL_P7)
  NimBLEDevice::setPower(ESP_PWR_LVL_P7);
#elif defined(ESP_PWR_LVL_P6)
  NimBLEDevice::setPower(ESP_PWR_LVL_P6);
#else
  // Fallback: numerischer Wert (0..9); nur nutzen falls Makros fehlen.
  NimBLEDevice::setPower(7);
#endif
  NimBLEDevice::setMTU(247); // größere MTU für weniger Chunks
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new RestartAdvServerCallbacks());
  NimBLEService* svc = server->createService(BLE_SERVICE_UUID);
  NimBLECharacteristic* chr = svc->createCharacteristic(
      BLE_CHARACTERISTIC_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  chr->setCallbacks(new CalendarCharCallbacks());
  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
  // Manche NimBLE-Versionen besitzen setScanResponse() nicht – Name trotzdem im ADV erzwingen
  // indem der Gerätename gesetzt wird (bereits durch init), optional nochmals:
  adv->setName("CalSync");
  adv->setAppearance(0x0000);
  adv->start();
  Serial.println("BLE bereit (Service: CalSync). Erster Chunk: LEN:<bytes>\\n...");
}

// ==== Your display pins (Lolin S2 Mini) ====
// #define EPD_PWR 6
// #define EPD_RST 8
// #define EPD_DC 9
// #define EPD_CS 10
// #define EPD_BUSY 7
// #define EPD_SCK 36
// #define EPD_MOSI 35
// ==== Your display pins (Xiao ESP32C3) ====
#define EPD_CS 7 // D5  orange
#define EPD_DC 4 // D2 green
#define EPD_RST 5 // D3 white
#define EPD_BUSY 3 // D1 violet
#define EPD_PWR 21 // D6 brown

#define EPD_SCK 6 // D4 yellow
#define EPD_MOSI 10 // D10 blue

GxEPD2_4C<GxEPD2_0579c_GDEY0579F51, GxEPD2_0579c_GDEY0579F51::HEIGHT> display(
    GxEPD2_0579c_GDEY0579F51(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// WiFi credentials will be loaded from /wifi.json (SPIFFS)

// ====== Sleep 30 minutes ======
static const uint64_t SLEEP_MIN = 30ULL;

RTC_DATA_ATTR char lastDate[11] = ""; // RTC memory for last date (YYYY-MM-DD)
RTC_DATA_ATTR uint32_t lastEventsHash = 0; // Hash der angezeigten Events dieses Tages

// Timeline constants
const int TIMELINE_START_HOUR = 8;
const int TIMELINE_END_HOUR = 18;
const int TIMELINE_Y_START = 65;
const int TIMELINE_Y_END = 780;
const int TIMELINE_HEIGHT = TIMELINE_Y_END - TIMELINE_Y_START;
const int TIMELINE_HOURS = TIMELINE_END_HOUR - TIMELINE_START_HOUR;
const float PX_PER_HOUR = (float)TIMELINE_HEIGHT / TIMELINE_HOURS;

// Helper: Convert ISO time string to Y coordinate on timeline
int timeToY(const String &isoTime)
{
  int t_pos = isoTime.indexOf('T');
  int hour = TIMELINE_START_HOUR, min = 0;
  if (t_pos > 0 && isoTime.length() > t_pos + 5)
  {
    hour = isoTime.substring(t_pos + 1, t_pos + 3).toInt();
    min = isoTime.substring(t_pos + 4, t_pos + 6).toInt();
  }
  return TIMELINE_Y_START + (int)(((hour - TIMELINE_START_HOUR) + min / 60.0f) * PX_PER_HOUR);
}

// Helper: Compare two date strings
bool isDateChanged(const char *current, const char *last)
{
  return strncmp(current, last, 10) != 0;
}
// Helper: Mount SPIFFS and print status
bool mountSPIFFS()
{
  if (!SPIFFS.begin(true))
  {
    Serial.println("Fehler: SPIFFS mounten fehlgeschlagen!");
    return false;
  }
  Serial.println("SPIFFS gemountet.");
  return true;
}

// Helper: Load file content as String
String loadFile(const char *path)
{
  File file = SPIFFS.open(path, "r");
  if (!file)
  {
    Serial.printf("Datei %s nicht gefunden!\n", path);
    return String();
  }
  String content = file.readString();
  file.close();
  return content;
}

// Helper: Parse calendar JSON
bool parseCalendarJson(const String &jsonStr, JsonDocument &doc)
{
  DeserializationError error = deserializeJson(doc, jsonStr);
  if (error)
  {
    Serial.println("Fehler beim Parsen der JSON!");
    return false;
  }
  return true;
}

// ==== WiFi credentials handling ====
struct WifiCred { String ssid; String pass; };

// Expected JSON format in /wifi.json (uploaded via SPIFFS data upload):
// [
//   { "ssid": "PrimaryNet", "password": "secretPW" },
//   { "ssid": "BackupNet",  "password": "backupPW" }
// ]
// The code will iterate in order and connect to the first AP that succeeds.
std::vector<WifiCred> loadWifiCredentials(const char* path = "/wifi.json") {
  std::vector<WifiCred> creds;
  String json = loadFile(path);
  if (json.isEmpty()) {
    Serial.println("Keine WiFi JSON geladen.");
    return creds;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    Serial.println("WiFi JSON Parse Fehler");
    return creds;
  }
  if (!doc.is<JsonArray>()) {
    Serial.println("WiFi JSON kein Array");
    return creds;
  }
  for (JsonObject o : doc.as<JsonArray>()) {
    const char* ssid = o["ssid"] | (const char*)nullptr;
    const char* pw = o["password"] | "";
    if (ssid && strlen(ssid)) {
      creds.push_back({String(ssid), String(pw)});
    }
  }
  Serial.printf("%u WiFi Credentials geladen.\n", (unsigned)creds.size());
  return creds;
}

bool connectAnyWifi(const std::vector<WifiCred>& creds, uint32_t perApTimeoutMs = 8000) {
  for (const auto& c : creds) {
    Serial.printf("Verbinde mit SSID '%s'...\n", c.ssid.c_str());
    WiFi.begin(c.ssid.c_str(), c.pass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < perApTimeoutMs) {
      delay(300);
      Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("Verbunden: %s  IP=%s\n", c.ssid.c_str(), WiFi.localIP().toString().c_str());
      return true;
    } else {
      Serial.printf("Fehlgeschlagen: %s\n", c.ssid.c_str());
      WiFi.disconnect(true);
      delay(200);
    }
  }
  return false;
}

// Helper: Get today's date as YYYY-MM-DD
String getTodayString()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("Zeit konnte nicht abgerufen werden!");
    return String();
  }
  char today[11];
  snprintf(today, sizeof(today), "%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
  return String(today);
}

// Helper: Find today's events in calendar JSON
struct Event
{
  String title;
  String start;
  String end;
  String location;
  String organizer;
  bool isImportant;
  bool isOnlineMeeting;
  bool isRecurring;
  bool isMoved;
  bool hasAttachments;
  bool isCanceled; // new
};

// FNV-1a 32-bit Hash für heutige Events (stabil, schnell, geringes Kollisionsrisiko für unseren Umfang)
static uint32_t computeEventsHash(const std::vector<struct Event>& events) {
  uint32_t h = 2166136261u; // FNV offset basis
  for (auto &e : events) {
    String line = e.start + "|" + e.end + "|" + e.title + "|" + e.location + "|" + e.organizer + "|" +
                  (e.isCanceled?"C":"-") + (e.isOnlineMeeting?"O":"-") + (e.isRecurring?"R":"-") + (e.isImportant?"I":"-");
    const char* p = line.c_str();
    while (*p) { h ^= (uint8_t)*p++; h *= 16777619u; }
    // Feldtrenner zwischen Events, damit (AB|C)(D) != (A)(B|CD)
    h ^= (uint8_t)'\n'; h *= 16777619u;
  }
  return h;
}

std::vector<Event> findTodaysEvents(JsonArray events, const String &today)
{
  std::vector<Event> todaysEvents;
  for (JsonObject evt : events)
  {
    const char *start = evt["start"];
    const char *end = evt["end"];
    String title = evt["summary"] | evt["subject"] | "(kein Titel)";
    String location = evt["location"] | "";
    if (location.startsWith("; ")) // truncate long URLs
      location = location.substring(2, location.length() - 2);
    // Perform replacements separately (String::replace returns void)
    location.replace("DE-", "");
    location.replace("HB-", "");
    location.replace("COC-", "");
    String organizer = evt["organizer"] | "";
    String startStr = String(start);
    String endStr = String(end);
    bool isImportant = evt["importance"] == "high" | false;
    bool isOnlineMeeting = evt["isOnlineMeeting"] | false;
    bool isRecurring = evt["isRecurring"] | false;
    bool isMoved = evt["isMoved"] | false;
    bool hasAttachments = evt["hasAttachments"] | false;
    const char* status = evt["status"] | evt["eventStatus"] | ""; // try multiple keys
    bool isCanceled = evt["isCancelled"] | false;
    if (strncmp(start, today.c_str(), 10) == 0)
    {
      todaysEvents.push_back({title, startStr, endStr, location, organizer, isImportant, isOnlineMeeting, isRecurring, isMoved, hasAttachments, isCanceled});
    }
  }
  return todaysEvents;
}

// Helper: Draw timeline axis
void drawTimelineAxis()
{
  for (int h = TIMELINE_START_HOUR; h <= TIMELINE_END_HOUR; h++)
  {
    int y = TIMELINE_Y_START + (int)((h - TIMELINE_START_HOUR) * PX_PER_HOUR);
    display.setFont(&FreeSans6pt7b);
    display.setCursor(0, y + 5);
    char buf[6];
    sprintf(buf, "%02d", h);
    display.print(buf);
    display.drawLine(17, y, display.width(), y, GxEPD_DARKGREY);
  }
}

// Helper: Draw events using external layout engine
// simple helper: wrap text within width using current font, returns y after drawing
int drawWrapped(int x, int y, int w, const String &text, int maxLines, int lineAdvance) {
  int line = 0;
  int cursorX = x; int cursorY = y;
  String current;
  int lastBreakPos = -1;
  for (int i = 0; i < (int)text.length(); ++i) {
    char c = text[i];
    if (c == '\n') {
      display.setCursor(cursorX, cursorY);
      display.print(current);
      current = ""; cursorY += lineAdvance; line++; if (line>=maxLines) return cursorY; continue;
    }
    current += c;
    int16_t bx, by; uint16_t bw,bh; display.getTextBounds(current, 0,0,&bx,&by,&bw,&bh);
    if (bw > (uint16_t)w) {
      // emit previous word
      int cutPos = current.lastIndexOf(' ');
      if (cutPos <= 0) cutPos = current.length()-1; // force cut
      String out = current.substring(0, cutPos);
      if (out.length()==0) out = current.substring(0, current.length()-1);
      display.setCursor(cursorX, cursorY);
      display.print(out);
      cursorY += lineAdvance; line++; if (line>=maxLines) return cursorY;
      current = current.substring(cutPos); current.trim();
    }
  }
  if (current.length()) { display.setCursor(cursorX, cursorY); display.print(current); cursorY += lineAdvance; }
  return cursorY;
}

// minutes->Y helper for segments
int minutesToY(int minutesFromMidnight)
{
  int hour = minutesFromMidnight / 60;
  int min = minutesFromMidnight % 60;
  float rel = ((hour - TIMELINE_START_HOUR) + min / 60.0f);
  return TIMELINE_Y_START + (int)(rel * PX_PER_HOUR);
}

void drawEvents(const std::vector<Event> &events)
{
  display.setFont(&FreeSansBold7pt7b);  
  display.setTextColor(GxEPD_BLACK);

  if (events.empty()) {
    display.setCursor(50, TIMELINE_Y_START + 20);
    display.print("Keine Termine heute.");
    return;
  }
  std::vector<CalLayoutInput> inputs; inputs.reserve(events.size());
  for (auto &e : events) inputs.push_back({e.start, e.end});
  auto boxes = computeCalendarLayout(inputs);

  const int xBase = 20;
  const int innerWidth = 248;
  const int gap = 4;

  for (auto &box : boxes) {
    const Event &evt = events[box.eventIndex];
    int yStart = timeToY(evt.start);
    int yEnd = timeToY(box.effectiveEnd);
    if (yEnd <= yStart)
      yEnd = yStart + 22;
    int box_w;
    if (box.groupColumns == 2) box_w = (innerWidth - gap)/2; else box_w = (innerWidth - gap*(box.groupColumns-1)) / box.groupColumns;
    int span = max(1, box.colSpan);
    int box_x = xBase + box.column * (box_w + gap);
    int box_total_w = box_w * span + gap * (span - 1);
    int box_y = yStart + 1;
    int box_h = max(22, yEnd - yStart - 2);

    // Cancelled style: white fill, yellow border; else yellow fill
    if (evt.isCanceled) {
      display.setFont(&FreeSans7pt7b);
      display.fillRect(box_x, box_y, box_total_w, box_h, GxEPD_WHITE);
      display.drawRect(box_x, box_y, box_total_w, box_h, GxEPD_YELLOW);
    } else {
      display.setFont(&FreeSansBold7pt7b);
      display.fillRect(box_x, box_y, box_total_w, box_h, GxEPD_YELLOW);
    }

    int textLeft = box_x + 4;
    int textWidth = box_total_w - 8;
    int cursorY = box_y + 12;
    cursorY = drawWrapped(textLeft, cursorY, textWidth, evt.title, 2, 14);
    display.setFont(&Font5x7Fixed);
    cursorY = drawWrapped(textLeft, cursorY, textWidth, evt.organizer, 1, 12);
    drawWrapped(textLeft, cursorY, textWidth, evt.location, 1, 12);

    int iconX = box_x + box_w - 14;
    if (evt.isRecurring) {
      if (evt.isMoved)
        display.drawBitmap(iconX, box_y + 1, epd_bitmap_series_mov, 13, 12, GxEPD_BLACK);
      else
        display.drawBitmap(iconX, box_y + 1, epd_bitmap_series, 12, 12, GxEPD_BLACK);
    }
    if (evt.isOnlineMeeting)
      display.drawBitmap(iconX, box_y + box_h - 12, epd_bitmap_Teams, 12, 12, GxEPD_BLACK);
    if (evt.hasAttachments)
      display.drawBitmap(iconX - 10, box_y + 2, epd_bitmap_attachment, 10, 12, GxEPD_BLACK);
    if (evt.isImportant)
      display.drawBitmap(box_x + 1, box_y + 5, epd_bitmap_important, 6, 11, GxEPD_RED);
  }
}

// Draw update time at bottom right using 4x5 fixed font
void drawUpdateTimestamp() {
  struct tm ti;
  if (!getLocalTime(&ti))
    return;
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", ti.tm_hour, ti.tm_min);
  display.setFont(&Font4x5Fixed);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  int x = display.width() - w - 4;
  int y = display.height() - 4; // baseline near bottom
  display.setTextColor(GxEPD_DARKGREY);
  display.setCursor(x, y);
  display.print(buf);
}

void getGermanDateHeader(String &weekdayOut, String &dateOut)
{
  static const char *WEEKDAY_DE[7] = {"Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag"};
  static const char *MONTH_DE[12] = {"Januar", "Februar", "März", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November", "Dezember"};
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    weekdayOut = "";
    dateOut = "";
    return;
  }
  int w = timeinfo.tm_wday;
  if (w < 0 || w > 6)
    w = 0;
  int m = timeinfo.tm_mon;
  if (m < 0 || m > 11)
    m = 0;
  weekdayOut = WEEKDAY_DE[w];
  // Some fonts may lack umlauts; optional fallback if needed
  // if (weekdayOut.indexOf("ä") >= 0) { /* keep it; font likely supports */ }
  dateOut = String(timeinfo.tm_mday) + ". " + MONTH_DE[m];
}

int battLvl(int Vbattf) {
  if (Vbattf > 4.2) return 11;
  if (Vbattf > 4.1) return 10;
  if (Vbattf > 4) return 9;
  if (Vbattf > 3.95) return 8;
  if (Vbattf > 3.90) return 7;
  if (Vbattf > 3.80) return 6;
  if (Vbattf > 3.70) return 5;
  if (Vbattf > 3.60) return 4;
  if (Vbattf > 3.50) return 3;
  if (Vbattf > 3.40) return 2;
  if (Vbattf > 3.30) return 1;
  return 0;
}

// Extrahierter Anzeige-Update-Code (aus setup)
bool updateCalendarFromJson(const String& jsonStr, bool forceRefresh) {
  Serial.println("Kalender-Update von JSON...");

  if (jsonStr.isEmpty()) return false;
  JsonDocument doc;
  if (!parseCalendarJson(jsonStr, doc)) return false;
  String today = getTodayString();
  if (today.isEmpty()) return false;

  bool dateChanged = isDateChanged(today.c_str(), lastDate);

  // Events extrahieren (aber erst Hash bilden, dann ggf. abbrechen)
  JsonArray events = doc.as<JsonArray>();
  std::vector<Event> todaysEvents = findTodaysEvents(events, today);

  uint32_t newHash = computeEventsHash(todaysEvents);
  #if CAL_HASH_DEBUG
    Serial.printf("Hash Check: date=%s events=%u new=0x%08lX prev=0x%08lX force=%d dateChanged=%d\n",
                  today.c_str(), (unsigned)todaysEvents.size(), (unsigned long)newHash, (unsigned long)lastEventsHash,
                  (int)forceRefresh, (int)dateChanged);
  #endif
  if (!forceRefresh && !dateChanged && newHash == lastEventsHash) {
    Serial.println("Unverändert (Datum & Events-Hash) – kein Redraw.");
    return true;
  }
  strncpy(lastDate, today.c_str(), sizeof(lastDate));
  lastEventsHash = newHash;

  display.setRotation(1);
  display.fillScreen(GxEPD_WHITE);

  String wday, dateLine;
  getGermanDateHeader(wday, dateLine);
  int headerH = 56;
  display.fillRect(0, 0, display.width(), headerH, GxEPD_RED);
  display.setTextColor(GxEPD_WHITE);
  display.setFont(&FreeSansBold12pt7b);
  display.setCursor(10, 22); display.print(wday);
  display.setCursor(10, 46); display.print(dateLine);

  // Status-Icons (optional unverändert) 
  uint32_t Vbatt = 0;
  for(int i = 0; i < 16; i++) {
    Vbatt = Vbatt + analogReadMilliVolts(A0); // ADC with correction   
  }
  float Vbattf = 2 * Vbatt / 16 / 1000.0;     // attenuation ratio 1/2, mV --> V
  Serial.println(Vbattf, 3);
  display.drawBitmap(270 - 18, 6, epd_bitmap_batt, 16, 9, GxEPD_WHITE);
  display.fillRect(270 - 18 + 2, 8, battLvl(Vbattf), 5, GxEPD_WHITE);

  display.drawBitmap(270 - 18 - 16, 3, epd_bitmap_bt, 11, 12, GxEPD_WHITE);

  display.setTextColor(GxEPD_BLACK);
  drawTimelineAxis();
  drawEvents(todaysEvents);
  drawUpdateTimestamp();
  display.display(true);
  Serial.println("Display aktualisiert (Kalender).");
  return true;
}

void setup()
{
  Serial.begin(115200);

  pinMode(A0, INPUT); 

  // Mount SPIFFS early (needed for wifi.json)
  if (!mountSPIFFS()) return;

  // BLE früh initialisieren (unabhängig von WiFi)
  initBLE();

  // Zeitzone immer konfigurieren, auch ohne WiFi/NTP.
  // Regel: CET (UTC+1) / CEST (UTC+2) mit Wechsel letzte So im März & Oktober.
  setenv("TZ","CET-1CEST,M3.5.0,M10.5.0/3",1); tzset();
  Serial.println("TZ gesetzt: CET/CEST");

  // (Optional: WiFi überspringen, wenn du rein BLE willst)
  // std::vector<WifiCred> creds = loadWifiCredentials();
  // if (!creds.empty() && connectAnyWifi(creds)) {
  //   configTime(0,0,"pool.ntp.org","time.nist.gov");
  //   setenv("TZ","CET-1CEST,M3.5.0,M10.5.0/3",1); tzset();
  //   struct tm tmpCheck; int retries=0;
  //   while (!getLocalTime(&tmpCheck) && retries < 20) { delay(200); retries++; }
  // } else {
  //   Serial.println("WiFi nicht verbunden – Zeit evtl. ungueltig bis späteres BLE-Update.");
  // }

  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, HIGH);
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init();

  // Start mit vorhandener Datei (falls vorhanden)
  String jsonStr = loadFile("/calendar-condensed.json");
  if (!jsonStr.isEmpty()) {
    updateCalendarFromJson(jsonStr, false);
  } else {
    Serial.println("Keine bestehende Kalender-Datei. Warte auf BLE Upload.");
  }

  // Deep Sleep erst wieder aktivieren, wenn BLE nicht ständig verfügbar sein soll.
  // (Sonst würde Verbindung abbrechen.)
}

void loop()
{
  if (bleTransferActive) {
    if (millis() - bleLastChunkMillis > BLE_TRANSFER_TIMEOUT_MS) {
      Serial.println("BLE Transfer Timeout – Reset.");
      bleTransferActive = false;
      bleExpectedLen = 0;
      if (bleBuffer) { free(bleBuffer); bleBuffer = nullptr; }
      bleBufferWritePos = 0;
    }
  }
  delay(200);
}