#include "secrets.h"
#include "bitmaps.h"
#include "weather_icons.h"
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ── Display definitions ────────────────────────────────────────────────────
// Shared SPI: DC=22, RST=21
//
// display1 = 4"   HH (hours)      CS=5,   BUSY=4
// display2 = 4"   MM (minutes)    CS=19,  BUSY=26
// display3 = 4"   DD (day)        CS=14,  BUSY=12
// display4 = 4"   MMM (month)     CS=32,  BUSY=33
// display5 = 7"   DDDE / weather  CS=25,  BUSY=27

GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT / 4> display1(GxEPD2_420_GDEY042T81(/*CS=*/ 5,  /*DC=*/ 22, /*RST=*/ 21, /*BUSY=*/ 4));
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT / 4> display2(GxEPD2_420_GDEY042T81(/*CS=*/ 19, /*DC=*/ 22, /*RST=*/ 21, /*BUSY=*/ 26));
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT / 4> display3(GxEPD2_420_GDEY042T81(/*CS=*/ 14, /*DC=*/ 22, /*RST=*/ 21, /*BUSY=*/ 12));
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT / 4> display4(GxEPD2_420_GDEY042T81(/*CS=*/ 32, /*DC=*/ 22, /*RST=*/ 21, /*BUSY=*/ 33));
GxEPD2_BW<GxEPD2_750_GDEY075T7, GxEPD2_750_GDEY075T7::HEIGHT / 2>  display5(GxEPD2_750_GDEY075T7( /*CS=*/ 25, /*DC=*/ 22, /*RST=*/ 21, /*BUSY=*/ 27));

U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// ── Weather helpers ────────────────────────────────────────────────────────

const char* weatherDescription(int code)
{
  if (code == 0)  return "Clear";
  if (code <= 3)  return "Partly Cloudy";
  if (code <= 48) return "Foggy";
  if (code <= 55) return "Drizzle";
  if (code <= 67) return "Rain";
  if (code <= 77) return "Snow";
  if (code <= 82) return "Showers";
  if (code <= 99) return "Thunderstorm";
  return "Unknown";
}

const char* windCardinal(int degrees)
{
  const char* directions[] = {
    "N","NNE","NE","ENE","E","ESE","SE","SSE",
    "S","SSW","SW","WSW","W","WNW","NW","NNW"
  };
  return directions[(int)((degrees + 11.25) / 22.5) % 16];
}

// ── Toronto coordinates ────────────────────────────────────────────────────

const float LAT = 43.6532;
const float LON = -79.3832;

// ── Bitmap dimensions ──────────────────────────────────────────────────────
// Digit bitmaps (display1, display2): two per panel side by side
const int DIGIT_BITMAP_WIDTH  = 200;
const int DIGIT_BITMAP_HEIGHT = 300;

// Day-of-week bitmaps (display5 upper half): full 7" panel width
const int DOW_BITMAP_WIDTH  = 800;
const int DOW_BITMAP_HEIGHT = 240;

// Month bitmaps (display4): full 4.2" panel width
const int MONTH_BITMAP_WIDTH  = 400;
const int MONTH_BITMAP_HEIGHT = 300;

// ── RTC memory (survives light sleep) ─────────────────────────────────────

RTC_DATA_ATTR int  lastMinute        = -1;
RTC_DATA_ATTR int  lastHour          = -1;
RTC_DATA_ATTR int  lastDay           = -1;
RTC_DATA_ATTR int  lastMonth         = -1;
RTC_DATA_ATTR int  lastSyncSlot      = -1;
RTC_DATA_ATTR int  updateCount       = 0;
RTC_DATA_ATTR bool lastWeatherValid  = false;
RTC_DATA_ATTR bool weatherChanged    = false;  // set after each sync, cleared after full refresh

RTC_DATA_ATTR int   weatherCode      = 0;
RTC_DATA_ATTR float rainSum          = 0;
RTC_DATA_ATTR float showersSum       = 0;
RTC_DATA_ATTR float snowfallSum      = 0;
RTC_DATA_ATTR float currentTemp      = 0;
RTC_DATA_ATTR float feelsLike        = 0;
RTC_DATA_ATTR float humidity         = 0;
RTC_DATA_ATTR float windSpeed        = 0;
RTC_DATA_ATTR int   windDirection    = 0;
RTC_DATA_ATTR float precipitation24h = 0;
RTC_DATA_ATTR float dayHigh          = 0;
RTC_DATA_ATTR float dayLow           = 0;
RTC_DATA_ATTR bool  weatherValid     = false;
RTC_DATA_ATTR bool  timeValid        = true;
RTC_DATA_ATTR char  weatherError[48] = "no attempt";
RTC_DATA_ATTR float pressure         = 0;
RTC_DATA_ATTR char  sunriseStr[6]    = "--:--";
RTC_DATA_ATTR char  sunsetStr[6]     = "--:--";

// ── Lookup tables ──────────────────────────────────────────────────────────

const char* dayNames[] = {
  "SUNDAY","MONDAY","TUESDAY","WEDNESDAY",
  "THURSDAY","FRIDAY","SATURDAY"
};

const char* monthNames[] = {
  "JAN","FEB","MAR","APR","MAY","JUN",
  "JUL","AUG","SEP","OCT","NOV","DEC"
};

const unsigned char* monthBitmaps[] = {
  epd_bitmap_VerdanaJAN, epd_bitmap_VerdanaFEB, epd_bitmap_VerdanaMAR,
  epd_bitmap_VerdanaAPR, epd_bitmap_VerdanaMAY, epd_bitmap_VerdanaJUN,
  epd_bitmap_VerdanaJUL, epd_bitmap_VerdanaAUG, epd_bitmap_VerdanaSEP,
  epd_bitmap_VerdanaOCT, epd_bitmap_VerdanaNOV, epd_bitmap_VerdanaDEC
};

// ── Day-of-week bitmap index ───────────────────────────────────────────────
// bitmaps.h allArray index by tm_wday (alphabetical order in new file):
// 14=FRI, 20=MON, 23=SAT, 25=SUN, 26=THURS, 27=TUES, 28=WED

int dowBitmapIndex(int tm_wday)
{
  const int indices[7] = {25, 20, 27, 28, 26, 14, 23}; // Sun=0 … Sat=6
  return indices[tm_wday];
}

// ── WiFi / NTP / Weather ───────────────────────────────────────────────────

bool fetchWeather()
{
  HTTPClient http;
  String url = String("https://api.open-meteo.com/v1/forecast?") +
    "latitude="  + String(LAT, 4) +
    "&longitude=" + String(LON, 4) +
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
               "wind_speed_10m,wind_direction_10m,weather_code,surface_pressure" +
    "&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,"
            "rain_sum,showers_sum,snowfall_sum,sunrise,sunset" +
    "&timezone=America%2FToronto&forecast_days=1";

  http.begin(url);
  http.setTimeout(8000);
  int httpCode = http.GET();
  if (httpCode != 200)
  {
    snprintf(weatherError, sizeof(weatherError), "HTTP %d  WiFi:%d", httpCode, WiFi.status());
    Serial.printf("Weather HTTP error: %d\n", httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, payload))
  {
    snprintf(weatherError, sizeof(weatherError), "JSON parse  WiFi:%d", WiFi.status());
    Serial.println("JSON parse failed");
    return false;
  }

  currentTemp      = doc["current"]["temperature_2m"];
  feelsLike        = doc["current"]["apparent_temperature"];
  humidity         = doc["current"]["relative_humidity_2m"];
  windSpeed        = doc["current"]["wind_speed_10m"];
  windDirection    = doc["current"]["wind_direction_10m"];
  weatherCode      = doc["current"]["weather_code"];
  pressure         = doc["current"]["surface_pressure"];
  dayHigh          = doc["daily"]["temperature_2m_max"][0];
  dayLow           = doc["daily"]["temperature_2m_min"][0];
  precipitation24h = doc["daily"]["precipitation_sum"][0];
  rainSum          = doc["daily"]["rain_sum"][0];
  showersSum       = doc["daily"]["showers_sum"][0];
  snowfallSum      = doc["daily"]["snowfall_sum"][0];

  // Parse sunrise/sunset — Open-Meteo returns "YYYY-MM-DDTHH:MM", keep HH:MM
  const char* sr = doc["daily"]["sunrise"][0];
  const char* ss = doc["daily"]["sunset"][0];
  if (sr && strlen(sr) >= 16) strncpy(sunriseStr, sr + 11, 5), sunriseStr[5] = '\0';
  if (ss && strlen(ss) >= 16) strncpy(sunsetStr,  ss + 11, 5), sunsetStr[5]  = '\0';

  return true;
}

void showError(const char* msg)
{
  // display6 retired — errors logged to Serial only
  Serial.printf("ERROR: %s\n", msg);
}

bool connectSyncAndFetch()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int attempts = 0;
  // Wait for association AND a valid DHCP address (not just layer-2 link)
  while (attempts < 20 &&
         (WiFi.status() != WL_CONNECTED ||
          WiFi.localIP() == IPAddress(0, 0, 0, 0)))
  {
    delay(500);
    attempts++;
  }
  Serial.printf("WiFi up after %d attempts, IP: %s\n",
                attempts, WiFi.localIP().toString().c_str());

  if (WiFi.status() != WL_CONNECTED)
  {
    String reason = "wifi failed: ";
    switch (WiFi.status())
    {
      case WL_NO_SSID_AVAIL:  reason += "no network found"; break;
      case WL_CONNECT_FAILED: reason += "wrong password";   break;
      case WL_DISCONNECTED:   reason += "disconnected";     break;
      default:                reason += String(WiFi.status()); break;
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    showError(reason.c_str());
    return false;
  }

  configTime(0, 0, "time.google.com", "time.cloudflare.com", "pool.ntp.org");
  setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
  tzset();

  struct tm timeinfo;
  int ntpAttempts = 0;
  while (!getLocalTime(&timeinfo) && ntpAttempts < 5)
  {
    ntpAttempts++;
    Serial.printf("NTP attempt %d of 5\n", ntpAttempts);
    showError(("NTP sync " + String(ntpAttempts) + "/5").c_str());
    delay(2000);
  }

  if (!getLocalTime(&timeinfo))
  {
    timeValid = false;
    showError("NTP failed");
  }
  else
  {
    timeValid = true;
    Serial.printf("Synced to: %02d:%02d:%02d\n",
      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  }

  weatherValid = false;
  for (int attempt = 1; attempt <= 3 && !weatherValid; attempt++)
  {
    weatherValid = fetchWeather();
    if (!weatherValid)
    {
      Serial.printf("Weather fetch failed (attempt %d/3)\n", attempt);
      if (attempt < 3) delay(2000 * attempt);  // 2s then 4s backoff
    }
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WiFi off");
  return true;
}

// ── Display rendering ──────────────────────────────────────────────────────

// Display 2 (right): HH — hours in 12hr format, two digit bitmaps side by side
void showHH(struct tm timeinfo, bool fullRefresh)
{
  int hour12 = timeinfo.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;

  display2.setRotation(0);
  if (fullRefresh) { display2.clearScreen(); display2.setFullWindow(); }
  else display2.setPartialWindow(0, 0, display2.width(), display2.height());

  display2.firstPage();
  do
  {
    display2.fillScreen(GxEPD_WHITE);
    display2.drawBitmap(0, 0,
      epd_bitmap_allArray[hour12 / 10],
      DIGIT_BITMAP_WIDTH, DIGIT_BITMAP_HEIGHT, GxEPD_BLACK);
    display2.drawBitmap(DIGIT_BITMAP_WIDTH, 0,
      epd_bitmap_allArray[hour12 % 10],
      DIGIT_BITMAP_WIDTH, DIGIT_BITMAP_HEIGHT, GxEPD_BLACK);
  }
  while (display2.nextPage());
}

// Display 1 (left): MM — minutes, two digit bitmaps side by side
// AM/PM overlaid in the lower-right corner (negative space of units digit)
void showMM(struct tm timeinfo, bool fullRefresh)
{
  const char* ampm = timeinfo.tm_hour < 12 ? "AM" : "PM";

  display1.setRotation(0);
  if (fullRefresh) { display1.clearScreen(); display1.setFullWindow(); }
  else display1.setPartialWindow(0, 0, display1.width(), display1.height());

  // measure AM/PM text width before entering the loop
  u8g2Fonts.begin(display1);
  u8g2Fonts.setFontMode(1);
  u8g2Fonts.setFont(u8g2_font_fub30_tf);
  int16_t ampmW = u8g2Fonts.getUTF8Width(ampm);
  int16_t ampmX = display1.width() - ampmW - 16;
  int16_t ampmY = display1.height() - 16;

  display1.firstPage();
  do
  {
    display1.fillScreen(GxEPD_WHITE);
    display1.drawBitmap(0, 0,
      epd_bitmap_allArray[timeinfo.tm_min / 10],
      DIGIT_BITMAP_WIDTH, DIGIT_BITMAP_HEIGHT, GxEPD_BLACK);
    display1.drawBitmap(DIGIT_BITMAP_WIDTH, 0,
      epd_bitmap_allArray[timeinfo.tm_min % 10],
      DIGIT_BITMAP_WIDTH, DIGIT_BITMAP_HEIGHT, GxEPD_BLACK);

    // bind u8g2 AFTER drawBitmap calls — avoids GFX state reset
    u8g2Fonts.begin(display1);
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setFont(u8g2_font_fub30_tf);
    u8g2Fonts.setForegroundColor(GxEPD_WHITE);
    u8g2Fonts.setBackgroundColor(GxEPD_BLACK);

    u8g2Fonts.setCursor(ampmX, ampmY);
    u8g2Fonts.print(ampm);
  }
  while (display1.nextPage());
}

// Maps WMO weather code + hour to weatherIcons[] index
// 0=clear day, 1=clear night, 2=clouds, 3=rain, 4=thunderstorm, 5=snow, 6=mist
int weatherIconIndex(int code, int hour)
{
  bool isDay = (hour >= 6 && hour < 20);
  if (code == 0)              return isDay ? 0 : 1;
  if (code <= 3)              return 2;
  if (code <= 48)             return 6;
  if (code <= 67 || (code >= 80 && code <= 82)) return 3;
  if (code <= 77 || (code >= 85 && code <= 86)) return 5;
  return 4;  // thunderstorm
}

// Nearest-neighbor scaled bitmap draw for display5 (white pixels on black bg)
static void drawScaledIcon(int x, int y, const uint8_t* bmp, int dstW, int dstH)
{
  for (int dy = 0; dy < dstH; dy++) {
    int sy = dy * 128 / dstH;
    for (int dx = 0; dx < dstW; dx++) {
      int sx = dx * 128 / dstW;
      int bit = sy * 128 + sx;
      if (pgm_read_byte(bmp + bit / 8) & (0x80 >> (bit % 8)))
        display5.drawPixel(x + dx, y + dy, GxEPD_WHITE);
    }
  }
}


// Teardrop water-drop icon, tip pointing up, circle centre at (cx, cy)
static void drawWaterDrop(int cx, int cy, int r)
{
  display5.fillCircle(cx, cy, r, GxEPD_WHITE);
  display5.fillTriangle(cx, cy - r - r, cx - r, cy, cx + r, cy, GxEPD_WHITE);
}

// Display 3: DD — day of month with leading zero, two digit bitmaps side by side
void showDD(struct tm timeinfo, bool fullRefresh)
{
  int day = timeinfo.tm_mday;  // 1–31; leading-zero handled by integer split

  display3.setRotation(0);
  if (fullRefresh) { display3.clearScreen(); display3.setFullWindow(); }
  else display3.setPartialWindow(0, 0, display3.width(), display3.height());

  display3.firstPage();
  do
  {
    display3.fillScreen(GxEPD_WHITE);
    display3.drawBitmap(0, 0,
      epd_bitmap_allArray[day / 10],
      DIGIT_BITMAP_WIDTH, DIGIT_BITMAP_HEIGHT, GxEPD_BLACK);
    display3.drawBitmap(DIGIT_BITMAP_WIDTH, 0,
      epd_bitmap_allArray[day % 10],
      DIGIT_BITMAP_WIDTH, DIGIT_BITMAP_HEIGHT, GxEPD_BLACK);
  }
  while (display3.nextPage());
}

// Display 4: MMM — full-panel month bitmap (400×300), same pattern as digit displays
void showMMM(struct tm timeinfo, bool fullRefresh)
{
  display4.setRotation(0);
  if (fullRefresh) { display4.clearScreen(); display4.setFullWindow(); }
  else display4.setPartialWindow(0, 0, display4.width(), display4.height());

  display4.firstPage();
  do
  {
    display4.fillScreen(GxEPD_WHITE);
    display4.drawBitmap(0, 0,
      monthBitmaps[timeinfo.tm_mon],
      MONTH_BITMAP_WIDTH, MONTH_BITMAP_HEIGHT, GxEPD_BLACK);
  }
  while (display4.nextPage());
}

// Display 5: DDDE
// Upper half: day-of-week bitmap
// Lower half: col1=weather icon+description, col2=temp/feels/HL, col3=wind text
void showDDDE(struct tm timeinfo, bool fullRefresh)
{
  display5.setRotation(0);
  if (fullRefresh) { display5.clearScreen(); display5.setFullWindow(); }
  else display5.setPartialWindow(0, 0, display5.width(), display5.height());

  const int W      = display5.width();        // 800
  const int TOP    = DOW_BITMAP_HEIGHT;       // 240
  const int ZONE_H = display5.height() - TOP; // 240

  // ── equal-thirds columns ─────────────────────────────────────────────────
  const int ICON_W     = 267;   // col1 right edge
  const int COL_CENTER = 400;   // col2 centre
  const int COL1_CTR   = 133;   // col1 centre
  const int COL3_CTR   = 667;   // col3 centre

  // ── col1: weather icon — 200px, shifted up slightly ──────────────────────
  const int ICON_DST = 200;
  const int ICON_X   = (ICON_W - ICON_DST) / 2;  // 33
  const int ICON_Y   = TOP + 18;                  // 8px higher than before

  // ── col2: temperature baselines ──────────────────────────────────────────
  const int C_TEMP = TOP + 98;    // logisoso78 top lands at ~260, clear of zone edge
  const int C_FEEL = TOP + 138;   // fub25, ~40px below temp baseline
  const int C_HL   = TOP + 200;   // fub25, dropped lower to use available space

  int16_t dowX = (W - DOW_BITMAP_WIDTH) / 2;

  display5.firstPage();
  do
  {
    display5.fillScreen(GxEPD_WHITE);

    // ── upper half: day-of-week bitmap ────────────────────────────────────
    display5.drawBitmap(dowX, 0,
      epd_bitmap_allArray[dowBitmapIndex(timeinfo.tm_wday)],
      DOW_BITMAP_WIDTH, DOW_BITMAP_HEIGHT, GxEPD_BLACK);

    // ── lower half: black background ──────────────────────────────────────
    display5.fillRect(0, TOP, W, ZONE_H, GxEPD_BLACK);

    u8g2Fonts.begin(display5);
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setForegroundColor(GxEPD_WHITE);
    u8g2Fonts.setBackgroundColor(GxEPD_BLACK);

    if (!weatherValid)
    {
      u8g2Fonts.setFont(u8g2_font_fub20_tf);
      u8g2Fonts.setCursor(COL_CENTER - u8g2Fonts.getUTF8Width("Weather unavailable") / 2, C_TEMP);
      u8g2Fonts.print("Weather unavailable");
      u8g2Fonts.setCursor(COL_CENTER - u8g2Fonts.getUTF8Width(weatherError) / 2, C_FEEL);
      u8g2Fonts.print(weatherError);
    }
    else
    {
      // ── col1: condition description ───────────────────────────────────────
      u8g2Fonts.setFont(u8g2_font_fub20_tf);
      {
        const char* desc = weatherDescription(weatherCode);
        u8g2Fonts.setCursor(COL1_CTR - u8g2Fonts.getUTF8Width(desc) / 2, TOP + 30);
        u8g2Fonts.print(desc);
      }

      // ── col1: weather icon scaled 128→200px ──────────────────────────────
      drawScaledIcon(ICON_X, ICON_Y,
        weatherIcons[weatherIconIndex(weatherCode, timeinfo.tm_hour)],
        ICON_DST, ICON_DST);

      u8g2Fonts.begin(display5);
      u8g2Fonts.setFontMode(1);
      u8g2Fonts.setForegroundColor(GxEPD_WHITE);
      u8g2Fonts.setBackgroundColor(GxEPD_BLACK);

      // ── col2: temperature ─────────────────────────────────────────────────
      char numStr[8];
      sprintf(numStr, "%.0f", currentTemp);
      const char* unitStr = "\xc2\xb0""C";
      u8g2Fonts.setFont(u8g2_font_logisoso78_tn);
      int16_t numW  = u8g2Fonts.getUTF8Width(numStr);
      u8g2Fonts.setFont(u8g2_font_fub30_tf);
      int16_t unitW = u8g2Fonts.getUTF8Width(unitStr);
      int16_t tempX = COL_CENTER - (numW + unitW) / 2;
      u8g2Fonts.setFont(u8g2_font_logisoso78_tn);
      u8g2Fonts.setCursor(tempX, C_TEMP);
      u8g2Fonts.print(numStr);
      u8g2Fonts.setFont(u8g2_font_fub30_tf);
      u8g2Fonts.setCursor(tempX + numW, C_TEMP - 22);
      u8g2Fonts.print(unitStr);

      // ── col2: feels like ──────────────────────────────────────────────────
      u8g2Fonts.setFont(u8g2_font_fub25_tf);
      char feelsBuf[24];
      sprintf(feelsBuf, "feels like %.0f\xc2\xb0""C", feelsLike);
      u8g2Fonts.setCursor(COL_CENTER - u8g2Fonts.getUTF8Width(feelsBuf) / 2, C_FEEL);
      u8g2Fonts.print(feelsBuf);

      // ── col2: high / low ──────────────────────────────────────────────────
      u8g2Fonts.setFont(u8g2_font_fub25_tf);
      char hlBuf[24];
      sprintf(hlBuf, "H:%.0f\xc2\xb0  L:%.0f\xc2\xb0""C", dayHigh, dayLow);
      u8g2Fonts.setCursor(COL_CENTER - u8g2Fonts.getUTF8Width(hlBuf) / 2, C_HL);
      u8g2Fonts.print(hlBuf);

      // ── col3: wind direction + speed ─────────────────────────────────────
      // direction at TOP+43, speed baseline at TOP+138 (aligns with C_FEEL)
      const char* cardDir = windCardinal(windDirection);
      u8g2Fonts.setFont(u8g2_font_fub20_tf);
      u8g2Fonts.setCursor(COL3_CTR - u8g2Fonts.getUTF8Width(cardDir) / 2, TOP + 43);
      u8g2Fonts.print(cardDir);

      char speedBuf[8];
      sprintf(speedBuf, "%.0f", windSpeed);
      u8g2Fonts.setFont(u8g2_font_logisoso78_tn);
      u8g2Fonts.setCursor(COL3_CTR - u8g2Fonts.getUTF8Width(speedBuf) / 2, TOP + 138);
      u8g2Fonts.print(speedBuf);

      u8g2Fonts.setFont(u8g2_font_fub20_tf);
      u8g2Fonts.setCursor(COL3_CTR - u8g2Fonts.getUTF8Width("km/h") / 2, TOP + 183);
      u8g2Fonts.print("km/h");
    }
  }
  while (display5.nextPage());
}



// ── Setup ──────────────────────────────────────────────────────────────────

void setup()
{
  Serial.begin(115200);

  // Shared RST: only the first init pulses RST (resets all displays at once).
  // Subsequent inits send each display's init commands via its own CS pin only.
  display1.init(115200, true,  2, false);
  display2.init(115200, false, 2, false);
  display3.init(115200, false, 2, false);
  display4.init(115200, false, 2, false);
  display5.init(115200, false, 2, false);

  connectSyncAndFetch();
}

// ── Loop ───────────────────────────────────────────────────────────────────

void loop()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("Failed to get local time");
    timeValid = false;
    showError("time lost - resyncing");
    bool resynced = connectSyncAndFetch();
    if (!resynced || !getLocalTime(&timeinfo))
    {
      showError("sync failed - sleep 5m");
      esp_sleep_enable_timer_wakeup(5ULL * 60 * 1000000);
      esp_light_sleep_start();
      return;
    }
  }

  // Sync every 30 minutes
  int currentSlot = timeinfo.tm_hour * 100 + (timeinfo.tm_min / 30) * 30;
  if (currentSlot != lastSyncSlot)
  {
    if (connectSyncAndFetch())
    {
      lastSyncSlot   = currentSlot;
      weatherChanged = true;   // trigger full refresh on next DDDE draw
    }
  }

  int currentMinute = timeinfo.tm_min;
  int currentHour   = timeinfo.tm_hour;

  if (currentMinute != lastMinute)
  {
    // Wake all displays from hibernate via SPI only — no RST pulse so the
    // controller retains its frame state and partial refresh keeps working
    display1.init(115200, false, 2, false);
    display2.init(115200, false, 2, false);
    display3.init(115200, false, 2, false);
    display4.init(115200, false, 2, false);
    display5.init(115200, false, 2, false);

    bool firstBoot  = (lastMinute == -1);  // force full refresh on cold start
    bool hourChanged = (currentHour != lastHour);
    bool hhFull   = firstBoot || hourChanged;
    bool mmFull   = firstBoot || (timeinfo.tm_min % 10 == 0);

    showHH(timeinfo, hhFull);
    showMM(timeinfo, mmFull);

    // DD and MMM only refresh when their value actually changes
    if (timeinfo.tm_mday != lastDay)   showDD(timeinfo, true);
    if (timeinfo.tm_mon  != lastMonth) showMMM(timeinfo, true);

    // DDDE only has content that changes on the hour, after a weather sync,
    // or when weather valid state changes — no point refreshing every minute
    bool dddeNeeded = firstBoot || hourChanged || weatherChanged
                                || (weatherValid != lastWeatherValid);
    if (dddeNeeded) showDDDE(timeinfo, true);  // always full refresh when called

    // Hibernate all display controllers — saves ~40mA continuous draw
    // (image is retained in pixel RAM; init() wakes them next cycle)
    display1.hibernate();
    display2.hibernate();
    display3.hibernate();
    display4.hibernate();
    display5.hibernate();

    lastMinute       = currentMinute;
    lastHour         = currentHour;
    lastDay          = timeinfo.tm_mday;
    lastMonth        = timeinfo.tm_mon;
    lastWeatherValid = weatherValid;
    weatherChanged   = false;
    updateCount++;
  }

  int secondsUntilNextMinute = 60 - timeinfo.tm_sec;
  esp_sleep_enable_timer_wakeup(secondsUntilNextMinute * 1000000ULL);
  esp_light_sleep_start();
}
