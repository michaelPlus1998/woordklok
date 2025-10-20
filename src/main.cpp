#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <FastLED.h>
#include <hardware/rtc.h>
#include <pico/util/datetime.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>

// ==================== HARDWARE CONFIGURATION ====================
#define LED_PIN     16
#define NUM_LEDS    56
#define BRIGHTNESS  64
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB
#define BRIGHTNESS_PIN 28
#define CONFIG_BUTTON_PIN 15

// ==================== CONFIGURATION CONSTANTS ====================
#define AP_SSID "WordClock-Setup"
#define AP_PASSWORD "Wordclock"
#define DNS_PORT 53
#define WEB_PORT 80
#define BUTTON_HOLD_TIME 3000
#define NTP_SYNC_INTERVAL 3600000    // 1 hour
#define WIFI_TIMEOUT 15000           // 15 seconds

// ==================== EEPROM CONFIGURATION ====================
#define EEPROM_SIZE 512
#define CONFIG_ADDRESS 0
#define CONFIG_VERSION 1

struct ConfigData {
    char ssid[64];
    char password[128];
    char ntpServer[64];
    int timezoneOffset;
    int brightness;
    bool configured;
    bool daylightSaving;
    uint32_t checksum;
};

// ==================== GLOBAL VARIABLES ====================
WiFiUDP ntpUDP;
NTPClient* timeClient = nullptr;
WebServer server(WEB_PORT);
DNSServer dnsServer;
CRGB leds[NUM_LEDS];

struct Config {
    char ssid[64] = "";
    char password[128] = "";
    char ntpServer[64] = "pool.ntp.org";
    int timezoneOffset = 0;
    int brightness = 64;
    bool configured = false;
    bool daylightSaving = true;
};

Config config;

// State variables
bool configMode = false;
bool wifiConnected = false;
unsigned long lastNTPSync = 0;
unsigned long buttonPressStart = 0;
bool buttonPressed = false;
bool rtcInitialized = false;
datetime_t currentTime;

// ==================== LED MAPPING FOR DUTCH WORDS ====================
const int UUR_LEDS[] = {2, 3, 4};
const int HET_IS_LEDS[] = {51, 52, 53, 54, 55};
const int AM_LED = 1;
const int PM_LED = 0;

const int HOUR_LEDS[][3] = {
    {5, -1, -1},      // 12/0 (TWAALF)
    {16, -1, -1},     // 1 (EEN)
    {15, -1, -1},     // 2 (TWEE) 
    {14, -1, -1},     // 3 (DRIE)
    {6, -1, -1},      // 4 (VIER)
    {7, -1, -1},      // 5 (VIJF)
    {8, -1, -1},      // 6 (ZES)
    {9, -1, -1},      // 7 (ZEVEN)
    {10, -1, -1},     // 8 (ACHT)
    {11, -1, -1},     // 9 (NEGEN)
    {12, -1, -1},     // 10 (TIEN)
    {13, -1, -1}      // 11 (ELF)
};

const int PRECIES_LEDS[] = {36, 37, 38, 39, 40, 41, 42};
const int RUIM_LEDS[] = {47, 48, 49, 50};
const int BIJNA_LEDS[] = {43, 44, 45, 46};
const int VIJF_MIN_LED = 35;
const int TIEN_MIN_LED = 34;
const int KWART_LEDS[] = {29, 30, 31, 32, 33};
const int VOOR_LEDS[] = {21, 22, 23, 24};
const int OVER_LEDS[] = {25, 26, 27, 28};
const int HALF_LEDS[] = {17, 18, 19, 20};

// ==================== FUNCTION DECLARATIONS ====================
void lightUpWord(const int* leds_array, int count);
void updateBrightness();
void startupAnimation();
void configModeAnimation();
uint32_t calculateChecksum(const ConfigData* data);
void setupEEPROM();
void loadConfiguration();
void saveConfiguration();
void resetConfiguration();
bool connectToWiFi();
void initializeNTPClient();
void cleanupWiFi();
void syncTimeWithNTP();
void updateRTCFromNTP();
void initializeRTC();
void checkNTPSync();
void getCurrentTime();
bool isDaylightSavingActive();
void displayTime();
void displayAlwaysOn();
void displayAMPM();
int getDisplayHour();
void displayHour_func(int hour);
void displayMinutes();
String generateCompactHTML(const String& title, const String& body, const String& script = "");
void setupWebServer();
void handleConfigButton();
void enterConfigMode();

// ==================== UTILITY FUNCTIONS ====================
void lightUpWord(const int* leds_array, int count) {
    for (int i = 0; i < count; i++) {
        leds[leds_array[i]] = CRGB::White;
    }
}

void updateBrightness() {
    static unsigned long lastBrightnessCheck = 0;
    if (millis() - lastBrightnessCheck > 1000) {
        int brightnessReading = analogRead(BRIGHTNESS_PIN);
        int brightness = map(brightnessReading, 0, 4095, 10, 255);
        FastLED.setBrightness(brightness);
        lastBrightnessCheck = millis();
    }
}

// ==================== ANIMATION FUNCTIONS ====================
void startupAnimation() {
    for (int i = 0; i < NUM_LEDS; i += 2) {
        leds[i] = CHSV(i * 255 / NUM_LEDS, 255, 255);
        if (i + 1 < NUM_LEDS) leds[i + 1] = CHSV((i + 1) * 255 / NUM_LEDS, 255, 255);
        FastLED.show();
        delay(25);
    }
    
    for (int brightness = 255; brightness >= 0; brightness -= 10) {
        FastLED.setBrightness(brightness);
        FastLED.show();
        delay(10);
    }
    
    FastLED.clear();
    FastLED.setBrightness(config.brightness);
    FastLED.show();
}

void configModeAnimation() {
    static unsigned long lastUpdate = 0;
    static int brightness = 0;
    static int direction = 1;
    
    if (millis() - lastUpdate > 100) {
        brightness += direction * 20;
        if (brightness >= 255) {
            brightness = 255;
            direction = -1;
        } else if (brightness <= 0) {
            brightness = 0;
            direction = 1;
        }
        
        fill_solid(leds, NUM_LEDS, CHSV(160, 255, brightness));
        FastLED.show();
        lastUpdate = millis();
    }
}

// ==================== TIME FUNCTIONS ====================
bool isDaylightSavingActive() {
    if (!config.daylightSaving) return false;
    
    int month = currentTime.month;
    int day = currentTime.day;
    
    if (month < 3 || month > 10) return false;
    if (month > 3 && month < 10) return true;
    
    if (month == 3) return day >= 25;
    if (month == 10) return day < 25;
    
    return false;
}

void updateRTCFromNTP() {
    if (timeClient == nullptr) return;
    
    unsigned long epochTime = timeClient->getEpochTime();
    time_t rawTime = epochTime;
    struct tm *timeInfo = gmtime(&rawTime);
    
    currentTime.year = timeInfo->tm_year + 1900;
    currentTime.month = timeInfo->tm_mon + 1;
    currentTime.day = timeInfo->tm_mday;
    currentTime.hour = timeInfo->tm_hour;
    currentTime.min = timeInfo->tm_min;
    currentTime.sec = timeInfo->tm_sec;
    currentTime.dotw = timeInfo->tm_wday;
    
    if (isDaylightSavingActive()) {
        currentTime.hour += 1;
        if (currentTime.hour >= 24) {
            currentTime.hour = 0;
        }
    }
    
    rtc_set_datetime(&currentTime);
    rtcInitialized = true;
}

void syncTimeWithNTP() {
    if (!wifiConnected || timeClient == nullptr) return;
    
    int attempts = 0;
    while (!timeClient->update() && attempts < 5) {
        timeClient->forceUpdate();
        delay(500);
        attempts++;
    }
    
    if (attempts < 5) {
        lastNTPSync = millis();
        updateRTCFromNTP();
    }
}

void initializeRTC() {
    rtc_init();
}

void checkNTPSync() {
    if (millis() - lastNTPSync > NTP_SYNC_INTERVAL) {
        syncTimeWithNTP();
    }
}

void getCurrentTime() {
    if (rtcInitialized && rtc_get_datetime(&currentTime)) {
        return;
    } else if (timeClient != nullptr) {
        unsigned long epochTime = timeClient->getEpochTime();
        epochTime += config.timezoneOffset;
        
        if (config.daylightSaving && isDaylightSavingActive()) {
            epochTime += 3600;
        }
        
        time_t rawTime = epochTime;
        struct tm *timeInfo = gmtime(&rawTime);
        
        currentTime.hour = timeInfo->tm_hour;
        currentTime.min = timeInfo->tm_min;
        currentTime.sec = timeInfo->tm_sec;
        currentTime.year = timeInfo->tm_year + 1900;
        currentTime.month = timeInfo->tm_mon + 1;
        currentTime.day = timeInfo->tm_mday;
        currentTime.dotw = timeInfo->tm_wday;
    }
}

// ==================== EEPROM FUNCTIONS ====================
uint32_t calculateChecksum(const ConfigData* data) {
    uint32_t checksum = 0;
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t i = 0; i < sizeof(ConfigData) - sizeof(uint32_t); i++) {
        checksum += bytes[i];
    }
    return checksum;
}

void setupEEPROM() {
    EEPROM.begin(EEPROM_SIZE);
}

void loadConfiguration() {
    Serial.println("Loading configuration from EEPROM...");
    
    ConfigData tempConfig;
    EEPROM.get(CONFIG_ADDRESS, tempConfig);
    
    uint32_t calculatedChecksum = calculateChecksum(&tempConfig);
    if (tempConfig.checksum != calculatedChecksum) {
        Serial.println("Invalid configuration checksum, using defaults");
        config.configured = false;
        return;
    }
    
    strlcpy(config.ssid, tempConfig.ssid, sizeof(config.ssid));
    strlcpy(config.password, tempConfig.password, sizeof(config.password));
    strlcpy(config.ntpServer, tempConfig.ntpServer, sizeof(config.ntpServer));
    config.timezoneOffset = tempConfig.timezoneOffset;
    config.brightness = tempConfig.brightness;
    config.configured = tempConfig.configured;
    config.daylightSaving = tempConfig.daylightSaving;
    
    Serial.println("Configuration loaded successfully");
}

void saveConfiguration() {
    Serial.println("Saving configuration to EEPROM...");
    
    ConfigData tempConfig = {0};
    
    strlcpy(tempConfig.ssid, config.ssid, sizeof(tempConfig.ssid));
    strlcpy(tempConfig.password, config.password, sizeof(tempConfig.password));
    strlcpy(tempConfig.ntpServer, config.ntpServer, sizeof(tempConfig.ntpServer));
    tempConfig.timezoneOffset = config.timezoneOffset;
    tempConfig.brightness = config.brightness;
    tempConfig.configured = true;
    tempConfig.daylightSaving = config.daylightSaving;
    tempConfig.checksum = calculateChecksum(&tempConfig);
    
    EEPROM.put(CONFIG_ADDRESS, tempConfig);
    EEPROM.commit();
    
    config.configured = true;
    Serial.println("Configuration saved to EEPROM successfully");
}

void resetConfiguration() {
    Serial.println("Resetting EEPROM configuration...");
    
    ConfigData emptyConfig = {0};
    emptyConfig.configured = false;
    emptyConfig.checksum = calculateChecksum(&emptyConfig);
    
    EEPROM.put(CONFIG_ADDRESS, emptyConfig);
    EEPROM.commit();
    
    memset(&config, 0, sizeof(config));
    config.timezoneOffset = 0;
    config.brightness = 64;
    strcpy(config.ntpServer, "pool.ntp.org");
    config.configured = false;
    config.daylightSaving = true;
    
    Serial.println("Configuration reset complete");
}

// ==================== WIFI FUNCTIONS ====================
bool connectToWiFi() {
    if (strlen(config.ssid) == 0) {
        return false;
    }
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssid, config.password);
    
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < WIFI_TIMEOUT) {
        delay(250);
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        return true;
    } else {
        wifiConnected = false;
        return false;
    }
}

void initializeNTPClient() {
    if (timeClient != nullptr) {
        timeClient->end();
        delete timeClient;
    }
    
    timeClient = new NTPClient(ntpUDP, config.ntpServer, config.timezoneOffset);
    timeClient->begin();
}

void cleanupWiFi() {
    if (timeClient != nullptr) {
        timeClient->end();
        delete timeClient;
        timeClient = nullptr;
    }
    
    ntpUDP.stop();
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    
    wifiConnected = false;
}

// ==================== DISPLAY FUNCTIONS ====================
void displayTime() {
    FastLED.clear();
    displayAlwaysOn();
    displayAMPM();
    
    int displayHour = getDisplayHour();
    displayHour_func(displayHour);
    displayMinutes();
}

void displayAlwaysOn() {
    for (int i = 0; i < 5; i++) {
        leds[HET_IS_LEDS[i]] = CRGB::Yellow;
    }
}

void displayAMPM() {
    int hour = currentHour;
    
    // Als minuten > 17, dan tonen we al het volgende uur
    // dus AM/PM moet ook voor dat uur zijn
    if (currentMinute > 17) {
        hour++;
        if (hour >= 24) hour = 0;
    }
    
    if (hour < 12) {
        leds[AM_LED] = CRGB::White;
    } else {
        leds[PM_LED] = CRGB::White;
    }
}

int getDisplayHour() {
    int hour = currentTime.hour;
    int minute = currentTime.min;
    
    if (hour == 0) hour = 12;
    else if (hour > 12) hour -= 12;
    
    if (minute > 17) {
        hour++;
        if (hour > 12) hour = 1;
    }
    
    return hour;
}

void displayHour_func(int hour) {
    if (hour < 1 || hour > 12) return;
    
    int hourIndex = (hour == 12) ? 0 : hour;
    
    for (int i = 0; i < 3; i++) {
        if (HOUR_LEDS[hourIndex][i] != -1) {
            leds[HOUR_LEDS[hourIndex][i]] = CRGB::White;
        }
    }
}

void displayUUR() {
    for (int i = 0; i < 3; i++) {
        leds[UUR_LEDS[i]] = CRGB::White;
    }
}


void displayMinutes() {
    int minute = currentTime.min;
    
    if (minute == 0) {
        displayUUR();
        lightUpWord(PRECIES_LEDS, 7);
    }
    else if (minute >= 1 && minute <= 2) {
        lightUpWord(RUIM_LEDS, 4);
        lightUpWord(OVER_LEDS, 4);
    }
    else if (minute >= 3 && minute <= 4) {
        lightUpWord(BIJNA_LEDS, 4);
        leds[VIJF_MIN_LED] = CRGB::White;
        lightUpWord(OVER_LEDS, 4);
    }
    else if (minute == 5) {
        lightUpWord(PRECIES_LEDS, 7);
        leds[VIJF_MIN_LED] = CRGB::White;
        lightUpWord(OVER_LEDS, 4);
    }
    else if (minute >= 6 && minute <= 7) {
        lightUpWord(RUIM_LEDS, 4);
        leds[VIJF_MIN_LED] = CRGB::White;
        lightUpWord(OVER_LEDS, 4);
    }
    else if (minute >= 8 && minute <= 9) {
        lightUpWord(BIJNA_LEDS, 4);
        leds[TIEN_MIN_LED] = CRGB::White;
        lightUpWord(OVER_LEDS, 4);
    }
    else if (minute == 10) {
        lightUpWord(PRECIES_LEDS, 7);
        leds[TIEN_MIN_LED] = CRGB::White;
        lightUpWord(OVER_LEDS, 4);
    }
    else if (minute >= 11 && minute <= 12) {
        lightUpWord(RUIM_LEDS, 4);
        leds[TIEN_MIN_LED] = CRGB::White;
        lightUpWord(OVER_LEDS, 4);
    }
    else if (minute >= 13 && minute <= 14) {
        lightUpWord(BIJNA_LEDS, 4);
        lightUpWord(KWART_LEDS, 5);
        lightUpWord(OVER_LEDS, 4);
    }
    else if (minute == 15) {
        lightUpWord(PRECIES_LEDS, 7);
        lightUpWord(KWART_LEDS, 5);
        lightUpWord(OVER_LEDS, 4);
    }
    else if (minute >= 16 && minute <= 17) {
        lightUpWord(RUIM_LEDS, 4);
        lightUpWord(KWART_LEDS, 5);
        lightUpWord(OVER_LEDS, 4);
    }
    else if (minute >= 18 && minute <= 19) {
        lightUpWord(BIJNA_LEDS, 4);
        leds[TIEN_MIN_LED] = CRGB::White;
        lightUpWord(VOOR_LEDS, 4);
        lightUpWord(HALF_LEDS, 4);
    }
    else if (minute == 20) {
        lightUpWord(PRECIES_LEDS, 7);
        leds[TIEN_MIN_LED] = CRGB::White;
        lightUpWord(VOOR_LEDS, 4);
        lightUpWord(HALF_LEDS, 4);
    }
    else if (minute >= 21 && minute <= 22) {
        lightUpWord(RUIM_LEDS, 4);
        leds[TIEN_MIN_LED] = CRGB::White;
        lightUpWord(VOOR_LEDS, 4);
        lightUpWord(HALF_LEDS, 4);
    }
    else if (minute >= 23 && minute <= 24) {
        lightUpWord(BIJNA_LEDS, 4);
        leds[VIJF_MIN_LED] = CRGB::White;
        lightUpWord(VOOR_LEDS, 4);
        lightUpWord(HALF_LEDS, 4);
    }
    else if (minute == 25) {
        lightUpWord(PRECIES_LEDS, 7);
        leds[VIJF_MIN_LED] = CRGB::White;
        lightUpWord(VOOR_LEDS, 4);
        lightUpWord(HALF_LEDS, 4);
    }
    else if (minute >= 26 && minute <= 27) {
        lightUpWord(RUIM_LEDS, 4);
        leds[VIJF_MIN_LED] = CRGB::White;
        lightUpWord(VOOR_LEDS, 4);
        lightUpWord(HALF_LEDS, 4);
    }
    else if (minute >= 28 && minute <= 29) {
        lightUpWord(BIJNA_LEDS, 4);
        lightUpWord(HALF_LEDS, 4);
    }
    else if (minute == 30) {
        lightUpWord(PRECIES_LEDS, 7);
        lightUpWord(HALF_LEDS, 4);
    }
    else if (minute >= 31 && minute <= 32) {
        lightUpWord(RUIM_LEDS, 4);
        lightUpWord(HALF_LEDS, 4);
    }
    else if (minute >= 33 && minute <= 34) {
        lightUpWord(BIJNA_LEDS, 4);
        leds[VIJF_MIN_LED] = CRGB::White;
        lightUpWord(OVER_LEDS, 4);
        lightUpWord(HALF_LEDS, 4);
    }
    else if (minute == 35) {
        lightUpWord(PRECIES_LEDS, 7);
        leds[VIJF_MIN_LED] = CRGB::White;
        lightUpWord(OVER_LEDS, 4);
        lightUpWord(HALF_LEDS, 4);
    }
    else if (minute >= 36 && minute <= 37) {
        lightUpWord(RUIM_LEDS, 4);
        leds[VIJF_MIN_LED] = CRGB::White;
        lightUpWord(OVER_LEDS, 4);
        lightUpWord(HALF_LEDS, 4);
    }
    else if (minute >= 38 && minute <= 39) {
        lightUpWord(BIJNA_LEDS, 4);
        leds[TIEN_MIN_LED] = CRGB::White;
        lightUpWord(OVER_LEDS, 4);
        lightUpWord(HALF_LEDS, 4);
    }
    else if (minute == 40) {
        lightUpWord(PRECIES_LEDS, 7);
        leds[TIEN_MIN_LED] = CRGB::White;
        lightUpWord(OVER_LEDS, 4);
        lightUpWord(HALF_LEDS, 4);
    }
    else if (minute >= 41 && minute <= 42) {
        lightUpWord(RUIM_LEDS, 4);
        leds[TIEN_MIN_LED] = CRGB::White;
        lightUpWord(OVER_LEDS, 4);
        lightUpWord(HALF_LEDS, 4);
    }
    else if (minute >= 43 && minute <= 44) {
        lightUpWord(BIJNA_LEDS, 4);
        lightUpWord(KWART_LEDS, 5);
        lightUpWord(VOOR_LEDS, 4);
    }
    else if (minute == 45) {
        lightUpWord(PRECIES_LEDS, 7);
        lightUpWord(KWART_LEDS, 5);
        lightUpWord(VOOR_LEDS, 4);
    }
    else if (minute >= 46 && minute <= 47) {
        lightUpWord(RUIM_LEDS, 4);
        lightUpWord(KWART_LEDS, 5);
        lightUpWord(VOOR_LEDS, 4);
    }
    else if (minute >= 48 && minute <= 49) {
        lightUpWord(BIJNA_LEDS, 4);
        leds[TIEN_MIN_LED] = CRGB::White;
        lightUpWord(VOOR_LEDS, 4);
    }
    else if (minute == 50) {
        lightUpWord(PRECIES_LEDS, 7);
        leds[TIEN_MIN_LED] = CRGB::White;
        lightUpWord(VOOR_LEDS, 4);
    }
    else if (minute >= 51 && minute <= 52) {
        lightUpWord(RUIM_LEDS, 4);
        leds[TIEN_MIN_LED] = CRGB::White;
        lightUpWord(VOOR_LEDS, 4);
    }
    else if (minute >= 53 && minute <= 54) {
        lightUpWord(BIJNA_LEDS, 4);
        leds[VIJF_MIN_LED] = CRGB::White;
        lightUpWord(VOOR_LEDS, 4);
    }
    else if (minute == 55) {
        lightUpWord(PRECIES_LEDS, 7);
        leds[VIJF_MIN_LED] = CRGB::White;
        lightUpWord(VOOR_LEDS, 4);
    }
    else if (minute >= 56 && minute <= 57) {
        lightUpWord(RUIM_LEDS, 4);
        leds[VIJF_MIN_LED] = CRGB::White;
        lightUpWord(VOOR_LEDS, 4);
    }
    else if (minute >= 58 && minute <= 59) {
        displayUUR();
        lightUpWord(BIJNA_LEDS, 4);
    }
}

// ==================== WEB INTERFACE ====================
String generateCompactHTML(const String& title, const String& body, const String& script) {
    String html = F("<!DOCTYPE html><html><head><title>");
    html += title;
    html += F("</title><meta name='viewport' content='width=device-width,initial-scale=1'>");
    html += F("<style>*{box-sizing:border-box}body{font:14px Arial;margin:20px;background:#f0f0f0}");
    html += F(".c{max-width:600px;margin:0 auto;background:#fff;padding:20px;border-radius:8px}");
    html += F("h1{color:#333;text-align:center;margin:0 0 20px}");
    html += F(".g{margin:15px 0}label{display:block;margin-bottom:5px;font-weight:bold}");
    html += F("input,select{width:100%;padding:8px;border:1px solid #ddd;border-radius:4px}");
    html += F("button{background:#4CAF50;color:#fff;padding:10px 20px;border:none;border-radius:4px;cursor:pointer;margin:5px}");
    html += F("button:hover{background:#45a049}.danger{background:#f44336}.warning{background:#ff9800}");
    html += F(".wifi{max-height:150px;overflow-y:auto;border:1px solid #ddd;padding:10px}");
    html += F(".wifi div{cursor:pointer;padding:5px;border-bottom:1px solid #eee}");
    html += F("</style></head><body><div class='c'>");
    html += body;
    html += F("</div>");
    if (script.length() > 0) {
        html += F("<script>");
        html += script;
        html += F("</script>");
    }
    html += F("</body></html>");
    return html;
}

void setupWebServer() {
    server.on("/", HTTP_GET, []() {
        String body = F("<h1>Word Clock Setup</h1>");
        body += F("<button onclick='scanWiFi()'>Scan WiFi Networks</button>");
        body += F("<div id='wifi' class='wifi' style='display:none'></div>");
        
        body += F("<form action='/save' method='post'>");
        body += F("<div class='g'><label>WiFi Network:</label><input name='ssid' value='");
        body += config.ssid;
        body += F("' required></div>");
        
        body += F("<div class='g'><label>WiFi Password:</label><input type='password' name='password' value='");
        body += config.password;
        body += F("'></div>");
        
        body += F("<div class='g'><label>Timezone:</label><select name='timezone'>");
        body += F("<option value='3600'");
        if (config.timezoneOffset == 3600) body += F(" selected");
        body += F(">Netherlands (UTC+1)</option>");
        body += F("<option value='0'");
        if (config.timezoneOffset == 0) body += F(" selected");
        body += F(">London (UTC+0)</option>");
        body += F("</select></div>");
        
        body += F("<div class='g'><label><input type='checkbox' name='daylight_saving' value='1'");
        if (config.daylightSaving) body += F(" checked");
        body += F("> Enable Daylight Saving Time</label></div>");
        
        body += F("<div class='g'><label>Brightness (10-255):</label><input type='number' name='brightness' min='10' max='255' value='");
        body += config.brightness;
        body += F("'></div>");
        
        body += F("<button type='submit'>Save & Restart</button></form>");
        
        body += F("<form action='/restart' method='post'><button type='submit' class='warning'>Restart</button></form>");
        body += F("<form action='/reset' method='post'><button type='submit' class='danger'>Factory Reset</button></form>");
        
        String script = F("function scanWiFi(){fetch('/scan').then(r=>r.text()).then(d=>{document.getElementById('wifi').innerHTML=d;document.getElementById('wifi').style.display='block'})}");
        script += F("function sel(s){document.querySelector('[name=ssid]').value=s}");
        
        server.send(200, F("text/html"), generateCompactHTML(F("Word Clock Setup"), body, script));
    });
    
    server.on("/scan", HTTP_GET, []() {
        String html = "";
        int n = WiFi.scanNetworks();
        
        for (int i = 0; i < min(n, 10); i++) {
            html += F("<div onclick='sel(\"");
            html += WiFi.SSID(i);
            html += F("\")'>");
            html += WiFi.SSID(i);
            html += F(" (");
            html += WiFi.RSSI(i);
            html += F("dBm)</div>");
        }
        
        server.send(200, F("text/html"), html);
    });
    
    server.on("/save", HTTP_POST, []() {
        strlcpy(config.ssid, server.arg("ssid").c_str(), sizeof(config.ssid));
        strlcpy(config.password, server.arg("password").c_str(), sizeof(config.password));
        config.timezoneOffset = server.arg("timezone").toInt();
        config.brightness = server.arg("brightness").toInt();
        config.daylightSaving = server.hasArg("daylight_saving");
        
        saveConfiguration();
        
        String body = F("<h1>Settings Saved!</h1><p>Restarting in <span id='c'>3</span> seconds...</p>");
        String script = F("let c=3;setInterval(()=>{c--;document.getElementById('c').textContent=c;if(c<=0)window.close()},1000)");
        
        server.send(200, F("text/html"), generateCompactHTML(F("Saved"), body, script));
        delay(1000);
        rp2040.restart();
    });
    
    server.on("/status", HTTP_GET, []() {
        String body = F("<h1>Status</h1>");
        body += F("<div style='margin:10px 0;padding:10px;background:#f9f9f9'>WiFi: ");
        body += wifiConnected ? F("Connected") : F("Disconnected");
        body += F("</div>");
        body += F("<div style='margin:10px 0;padding:10px;background:#f9f9f9'>Time: ");
        getCurrentTime();
        body += String(currentTime.hour) + ":";
        if (currentTime.min < 10) body += "0";
        body += String(currentTime.min);
        body += F("</div>");
        
        server.send(200, F("text/html"), generateCompactHTML(F("Status"), body));
    });
    
    server.on("/reset", HTTP_POST, []() {
        resetConfiguration();
        server.send(200, F("text/html"), generateCompactHTML(F("Reset"), F("<h1>Factory Reset Complete</h1><p>Restarting...</p>")));
        delay(1000);
        rp2040.restart();
    });
    
    server.on("/restart", HTTP_POST, []() {
        server.send(200, F("text/html"), generateCompactHTML(F("Restart"), F("<h1>Restarting...</h1>")));
        delay(1000);
        rp2040.restart();
    });
    
    server.onNotFound([]() {
        server.sendHeader("Location", "/");
        server.send(302);
    });
}

// ==================== BUTTON HANDLING ====================
void handleConfigButton() {
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck < 50) return;  // Debounce
    lastCheck = millis();
    
    bool currentState = digitalRead(CONFIG_BUTTON_PIN) == LOW;
    
    if (currentState && !buttonPressed) {
        buttonPressed = true;
        buttonPressStart = millis();
    } else if (!currentState && buttonPressed) {
        buttonPressed = false;
        if (millis() - buttonPressStart >= BUTTON_HOLD_TIME) {
            enterConfigMode();
        }
    }
}

void enterConfigMode() {
    cleanupWiFi();
    configMode = true;
    
    // Immediately start showing config mode animation
    configModeAnimation();
    FastLED.show();
    
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    setupWebServer();
    server.begin();
}

// ==================== SETUP ====================
void setup() {
    Serial.begin(9600);
    
    setupEEPROM();
    
    pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
    analogReadResolution(12);
    
    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
    FastLED.setBrightness(BRIGHTNESS);
    
    loadConfiguration();
    initializeRTC();
    startupAnimation();
    
    if (!config.configured) {
        enterConfigMode();
    } else {
        if (connectToWiFi()) {
            initializeNTPClient();
            syncTimeWithNTP();
        } else {
            enterConfigMode();
        }
    }
}

// ==================== MAIN LOOP ====================
void loop() {
    handleConfigButton();
    
    if (configMode) {
        dnsServer.processNextRequest();
        server.handleClient();
        configModeAnimation();
    } else {
        static unsigned long lastTimeUpdate = 0;
        
        updateBrightness();
        
        if (wifiConnected && timeClient != nullptr) {
            checkNTPSync();
        }
        
        // Only update time display once per second
        if (millis() - lastTimeUpdate > 1000) {
            getCurrentTime();
            displayTime();
            FastLED.show();
            lastTimeUpdate = millis();
        }
    }
    
    delay(10);
}
