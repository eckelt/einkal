
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

// ==== Your display pins (as in your working demo) ====
#define EPD_PWR   6
#define EPD_RST   8
#define EPD_DC    9
#define EPD_CS   10
#define EPD_BUSY  7
#define EPD_SCK  36
#define EPD_MOSI 35

GxEPD2_4C<GxEPD2_0579c_GDEY0579F51, GxEPD2_0579c_GDEY0579F51::HEIGHT> display(
  GxEPD2_0579c_GDEY0579F51(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

// ====== WiFi + ICS URL ======

// ====== Sleep 30 minutes ======
static const uint64_t SLEEP_MIN = 30ULL;

// Hilfsfunktion: Wandelt ISO-Zeitstring in y-Koordinate auf der Timeline
const int TIMELINE_START_HOUR = 8;
const int TIMELINE_END_HOUR = 18;
const int TIMELINE_Y_START = 60;
const int TIMELINE_Y_END = 760;
const int TIMELINE_HEIGHT = TIMELINE_Y_END - TIMELINE_Y_START;
const int TIMELINE_HOURS = TIMELINE_END_HOUR - TIMELINE_START_HOUR;
const float PX_PER_HOUR = (float)TIMELINE_HEIGHT / TIMELINE_HOURS;

int timeToY(const String& isoTime) {
  // isoTime: 'YYYY-MM-DDTHH:MM:SS+ZZZZ'
  int t_pos = isoTime.indexOf('T');
  int hour = TIMELINE_START_HOUR, min = 0;
  if (t_pos > 0 && isoTime.length() > t_pos+5) {
    hour = isoTime.substring(t_pos+1, t_pos+3).toInt();
    min = isoTime.substring(t_pos+4, t_pos+6).toInt();
  }
  return TIMELINE_Y_START + (int)(((hour - TIMELINE_START_HOUR) + min/60.0f) * PX_PER_HOUR);
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  // === Connect WiFi ===
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Verbinde mit WiFi...");
  int wifiTries = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTries < 20) {
    delay(500);
    Serial.print(".");
    wifiTries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi verbunden!");
  } else {
    Serial.println("\nWiFi Verbindung fehlgeschlagen!");
    return;
  }

  // === Zeit per NTP holen ===
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  if (!SPIFFS.begin(true)) {
    Serial.println("Fehler: SPIFFS mounten fehlgeschlagen!");
    return;
  } else {
    Serial.println("SPIFFS gemountet.");
  }

  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, 1);

  // Init display early (keeps stable)
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init();

  // === Load calendar-condensed.json ===
  File file = SPIFFS.open("/calendar-condensed.json", "r");
  if (!file) {
    Serial.println("calendar-condensed.json nicht gefunden!");
    return;
  }
  String jsonStr = file.readString();
  file.close();

  // === Parse JSON ===
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonStr);
  if (error) {
    Serial.println("Fehler beim Parsen der JSON!");
    return;
  }
  Serial.println(jsonStr);

  // === Get today's date ===
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Zeit konnte nicht abgerufen werden!");
    return;
  }
  char today[11]; // YYYY-MM-DD\0
  snprintf(today, sizeof(today), "%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

  // === Find today's events ===
  JsonArray events = doc.as<JsonArray>();
  struct Event {
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
  std::vector<Event> todaysEvents;
  for (JsonObject evt : events) {
    const char* date = evt["date"];
    const char* start = evt["start"];
    const char* end = evt["end"];
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
    if (strncmp(start, today, 10) == 0) {
        todaysEvents.push_back({title, startStr, endStr, location, organizer, isImportant, isOnlineMeeting, isRecurring, isMoved, hasAttachments});
      }
  }

  // === Display events ===
  display.setRotation(1);
  display.setFont(&FreeSansBold12pt7b);
  display.fillScreen(GxEPD_WHITE);
  display.setCursor(80, 25);
  display.fillRect(0, 0, display.width(), 40, GxEPD_RED);
  display.setTextColor(GxEPD_WHITE);
  display.print(today);
  display.drawBitmap(220, 8, epd_bitmap_wifi1, 12, 10, GxEPD_WHITE);
  display.setCursor(233, 16);
  display.setFont(&FreeSans6pt7b);
  display.print(WiFi.SSID());
  display.setTextColor(GxEPD_BLACK);
  // Timeline: 8-18 Uhr, y=60 bis y=360 (300px für 10h)

  // Achse zeichnen
  for (int h = TIMELINE_START_HOUR; h <= TIMELINE_END_HOUR; h++) {
    int y = TIMELINE_Y_START + (int)((h - TIMELINE_START_HOUR) * PX_PER_HOUR);
    display.setFont(&FreeSans6pt7b);
    display.setCursor(0, y+5);
    char buf[6];
    sprintf(buf, "%02d", h);
    display.print(buf);
    display.drawLine(17, y, display.width(), y, GxEPD_DARKGREY);
  }

  // Termine platzieren
  display.setFont(&FreeSansBold7pt7b);
  if (todaysEvents.size() == 0) {
  display.setCursor(50, TIMELINE_Y_START + 20);
    display.print("Keine Termine heute.");
  } else {
    for (const auto& evt : todaysEvents) {
      // Endzeit aus evt.end (falls vorhanden, sonst +1h)
        String endStr = evt.end.length() > 0 ? evt.end : evt.start;
        if (endStr == evt.start) {
          // +1h zum Start
          int t_pos = evt.start.indexOf('T');
          int hour = TIMELINE_START_HOUR, min = 0;
          if (t_pos > 0 && evt.start.length() > t_pos+5) {
            hour = evt.start.substring(t_pos+1, t_pos+3).toInt();
            min = evt.start.substring(t_pos+4, t_pos+6).toInt();
          }
          char buf[25];
          sprintf(buf, "%04d-%02d-%02dT%02d:%02d:00+0000", evt.start.substring(0,4).toInt(), evt.start.substring(5,7).toInt(), evt.start.substring(8,10).toInt(), hour+1, min);
          endStr = String(buf);
      }
      int y_start = timeToY(evt.start);
      int y_end = timeToY(endStr);
      int box_x = 20;
      int box_y = y_start;
      int box_w = 248;
      int box_h = max(22, y_end - y_start);
      // 2px Border
      display.fillRect(box_x, box_y, box_w, box_h, GxEPD_YELLOW);

      // Text: Zeit und Titel (klein)
      display.setFont(&FreeSansBold7pt7b);
      display.setCursor(box_x+6, box_y+15);
      display.write(evt.title.substring(0, 32).c_str());

      // Organisator (zweite Zeile, noch kleiner)
      display.setFont(&FreeSans6pt7b);
      display.setCursor(box_x+6, box_y+30);
      display.write(evt.organizer.c_str());

      // Location (dritte Zeile, noch kleiner)
      display.setCursor(box_x+6, box_y+42);
      display.write(evt.location.substring(0, 32).c_str());

      if (evt.isRecurring) {
        if (evt.isMoved) 
          display.drawBitmap(box_x+235, box_y+1, epd_bitmap_series_mov, 13, 12, GxEPD_BLACK);
        else
          display.drawBitmap(box_x+235, box_y+1, epd_bitmap_series, 12, 12, GxEPD_BLACK);
      }
      if (evt.isOnlineMeeting)
        display.drawBitmap(box_x+235, box_y+box_h-12, epd_bitmap_Teams, 12, 12, GxEPD_BLACK);
      if (evt.hasAttachments)
        display.drawBitmap(box_x+225, box_y+2, epd_bitmap_attachment, 10, 12, GxEPD_BLACK);
    
      if (evt.isImportant) 
        display.drawBitmap(box_x+1, box_y+5, epd_bitmap_important, 6, 11, GxEPD_RED);
    }
  }

  display.display(true);
}

void loop() {}