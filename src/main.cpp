#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_4C.h>
#include <epd4c/GxEPD2_0579c_GDEY0579F51.h>

// Pins: identical to your working Waveshare demo wiring
#define EPD_PWR   6
#define EPD_RST   8
#define EPD_DC    9
#define EPD_CS   10
#define EPD_BUSY  7   // BUSY active again (works in your demo)
#define EPD_SCK  36   // CLK
#define EPD_MOSI 35   // DIN

// GxEPD2 instance for 5.79" 4-Color (F51)
GxEPD2_4C<GxEPD2_0579c_GDEY0579F51, GxEPD2_0579c_GDEY0579F51::HEIGHT> display(
  GxEPD2_0579c_GDEY0579F51(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

void setup() {
  Serial.begin(115200);
  delay(1500); // give time to open Serial Monitor

  // IMPORTANT: Physically wire both VCC and PWR of the display to 5V (VBUS), and GND to GND

  // Prepare control pins
  pinMode(EPD_CS, OUTPUT); digitalWrite(EPD_CS, HIGH);
  pinMode(EPD_DC, OUTPUT);
  pinMode(EPD_RST, OUTPUT);

  // Hard reset before init (matches your demo behaviour)
  digitalWrite(EPD_RST, LOW);  delay(200);
  digitalWrite(EPD_RST, HIGH); delay(200);

    pinMode(EPD_PWR, OUTPUT);
    digitalWrite(EPD_PWR, 1);

  // Start SPI on the exact pins you use in the demo (use the default SPI instance)
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);

  // Tell GxEPD2 to use this SPI instance and conservative settings
  display.epd2.selectSPI(SPI, SPISettings(1000000, MSBFIRST, SPI_MODE0));

  Serial.println("GxEPD2 init...");
  display.init(115200 /*diag*/, true /*initial_refresh*/, 0 /*diag_level*/, true /*reset*/);

  display.setRotation(1);
  display.setFullWindow();

  // Then white background + text
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(20, 60);
    display.print("Hello GxEPD2");
  } while (display.nextPage());
  delay(2000);
  do {
    display.setTextColor(GxEPD_RED);
    display.setCursor(20, 80);
    display.print("Success!!");
  } while (display.nextPage());

  Serial.println("done");
}

void loop() {}