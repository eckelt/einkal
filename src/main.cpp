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
#include <Fonts/FreeSans6pt7b.h>
#include <Icons.h>
#include <time.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <CalLayout.h>  // local library in lib/CalLayout/

// ==== Your display pins (as in your working demo) ====
#define EPD_PWR 6
#define EPD_RST 8
#define EPD_DC 9
#define EPD_CS 10
#define EPD_BUSY 7
#define EPD_SCK 36
#define EPD_MOSI 35

GxEPD2_4C<GxEPD2_0579c_GDEY0579F51, GxEPD2_0579c_GDEY0579F51::HEIGHT> display(
    GxEPD2_0579c_GDEY0579F51(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// WiFi credentials will be loaded from /wifi.json (SPIFFS)

// ====== Sleep 30 minutes ======
static const uint64_t SLEEP_MIN = 30ULL;

RTC_DATA_ATTR char lastDate[11] = ""; // RTC memory for last date (YYYY-MM-DD)

// Timeline constants
const int TIMELINE_START_HOUR = 8;
const int TIMELINE_END_HOUR = 18;
const int TIMELINE_Y_START = 60;
const int TIMELINE_Y_END = 760;
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
};

std::vector<Event> findTodaysEvents(JsonArray events, const String &today)
{
  std::vector<Event> todaysEvents;
  for (JsonObject evt : events)
  {
    const char *start = evt["start"];
    const char *end = evt["end"];
    String title = evt["summary"] | evt["subject"] | "(kein Titel)";
    String location = evt["location"] | "";
    String organizer = evt["organizer"] | "";
    String startStr = String(start);
    String endStr = String(end);
    bool isImportant = evt["importance"] == "high" | false;
    bool isOnlineMeeting = evt["isOnlineMeeting"] | false;
    bool isRecurring = evt["isRecurring"] | false;
    bool isMoved = evt["isMoved"] | false;
    bool hasAttachments = evt["hasAttachments"] | false;
    if (strncmp(start, today.c_str(), 10) == 0)
    {
      todaysEvents.push_back({title, startStr, endStr, location, organizer, isImportant, isOnlineMeeting, isRecurring, isMoved, hasAttachments});
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
void drawEvents(const std::vector<Event> &events)
{
  display.setFont(&FreeSansBold7pt7b);
  if (events.empty()) {
    display.setCursor(50, TIMELINE_Y_START + 20);
    display.print("Keine Termine heute.");
    return;
  }

  // Build layout input
  std::vector<CalLayoutInput> inputs;
  inputs.reserve(events.size());
  for (auto &e : events) inputs.push_back({e.start, e.end});

  auto layout = computeCalendarLayout(inputs);

  const int xBase = 20;
  const int innerWidth = 248;
  const int gap = 4;

  for (auto &box : layout) {
    const Event &evt = events[box.eventIndex];
    // Compute y positions using original start and effective end
    int yStart = timeToY(evt.start);
    int yEnd = timeToY(box.effectiveEnd);
    if (yEnd <= yStart) yEnd = yStart + 22;

    int cols = box.groupColumns;
    int box_w;
    if (cols == 2) {
      box_w = (innerWidth - gap) / 2; // half width rule
    } else {
      box_w = (innerWidth - gap * (cols - 1)) / cols;
    }
    int box_x = xBase + box.column * (box_w + gap);
    int box_y = yStart + 1;
    int box_h = max(22, yEnd - yStart - 2);

    display.fillRect(box_x, box_y, box_w, box_h, GxEPD_YELLOW);

    display.setFont(&FreeSansBold7pt7b);
    display.setCursor(box_x + 4, box_y + 14);
    int maxChars = (box_w / 6) * 2; // crude fit heuristic
    display.write(evt.title.substring(0, max(4, maxChars)).c_str());

    display.setFont(&FreeSans6pt7b);
    display.setCursor(box_x + 4, box_y + 28);
    display.write(evt.organizer.substring(0, max(4, maxChars)).c_str());

    display.setCursor(box_x + 4, box_y + 40);
    display.write(evt.location.substring(0, max(4, maxChars)).c_str());

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

void setup()
{
  Serial.begin(115200);

  // Mount SPIFFS early (needed for wifi.json)
  if (!mountSPIFFS()) return;

  // Load and connect to first available WiFi from list
  std::vector<WifiCred> creds = loadWifiCredentials();
  if (creds.empty()) {
    Serial.println("Keine WiFi Zugangsdaten gefunden. Gehe schlafen.");
    esp_sleep_enable_timer_wakeup(5ULL * 60ULL * 1000000ULL);
    esp_deep_sleep_start();
  }
  if (!connectAnyWifi(creds)) {
    Serial.println("Keine Verbindung zu den konfigurierten WLANs möglich. Schlafe 5min.");
    esp_sleep_enable_timer_wakeup(5ULL * 60ULL * 1000000ULL);
    esp_deep_sleep_start();
  }

  // Zeit per NTP holen
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // SPIFFS already mounted above

  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, 1);
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init();

  String jsonStr = loadFile("/calendar-condensed.json");
  if (jsonStr.isEmpty())
    return;

  JsonDocument doc;
  if (!parseCalendarJson(jsonStr, doc))
    return;
  Serial.println(jsonStr);

  String today = getTodayString();
  if (today.isEmpty())
    return;

  // Only update display if date changed
  if (isDateChanged(today.c_str(), lastDate))
  {
    strncpy(lastDate, today.c_str(), sizeof(lastDate));
    JsonArray events = doc.as<JsonArray>();
    std::vector<Event> todaysEvents = findTodaysEvents(events, today);

    // Display events
    display.setRotation(1);
    display.setFont(&FreeSansBold12pt7b);
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(80, 25);
    display.fillRect(0, 0, display.width(), 40, GxEPD_RED);
    display.setTextColor(GxEPD_WHITE);
    display.print(today);
    int16_t x1, y1;
    uint16_t w, h;
    display.setFont(&FreeSans6pt7b);
    display.getTextBounds(WiFi.SSID(), 0, 0, &x1, &y1, &w, &h);
    Serial.printf("Text bounds test %d x %d\n", w, h);
    display.drawBitmap(270-w-14, 4, epd_bitmap_wifi1, 12, 10, GxEPD_WHITE);
    display.setCursor(270-w-2, 12);
    display.print(WiFi.SSID());
    display.setTextColor(GxEPD_BLACK);

    drawTimelineAxis();
    drawEvents(todaysEvents);

    display.display(true);
  }
  else
  {
    Serial.println("Date unchanged, skipping display update.");
  }

  // Deep sleep for 30 minutes
  Serial.println("Going to deep sleep...");
  esp_sleep_enable_timer_wakeup(SLEEP_MIN * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {}