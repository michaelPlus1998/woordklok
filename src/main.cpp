#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <FastLED.h>
#include <hardware/rtc.h>
#include <hardware/watchdog.h>
#include <pico/util/datetime.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>

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
#define NTP_SYNC_INTERVAL 1800000
#define WIFI_TIMEOUT 30000

// ==================== OTA UPDATE CONFIGURATION ====================
#define FIRMWARE_VERSION "1.0.1"
#define GITHUB_USER "michaelPlus1998"        // ← VERANDER DIT!
#define GITHUB_REPO "woordklok"           // ← VERANDER DIT!
#define GITHUB_API_URL "https://api.github.com/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest"
#define UPDATE_CHECK_INTERVAL 86400000
#define UPDATE_FILE_PATH "/update.uf2"

// ==================== EEPROM CONFIGURATION ====================
#define EEPROM_SIZE 512
#define CONFIG_ADDRESS 0

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

// OTA State variables
bool updateAvailable = false;
bool updateInProgress = false;
String latestVersion = "";
String downloadUrl = "";
unsigned long lastUpdateCheck = 0;
int updateProgress = 0;

// ==================== LED MAPPING FOR DUTCH WORDS ====================
const int HET_LEDS[] = {2, 3, 4};
const int IS_LEDS[] = {51, 52, 53, 54, 55};
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
// Utility functions
void lightUpWord(const int* leds_array, int count);
void updateBrightness();

// Animation functions  
void startupAnimation();
void configModeAnimation();
void showUpdateAnimation();
void showUpdateProgress(int progress);
void showUpdateSuccess();
void showUpdateError();

// Configuration functions
uint32_t calculateChecksum(const ConfigData* data);
void setupEEPROM();
void loadConfiguration();
void saveConfiguration();
void resetConfiguration();

// Hardware functions
void cleanupBeforeReset();
void performHardwareReset();

// WiFi functions
bool connectToWiFi();
void initializeNTPClient();
void cleanupWiFi();

// Time functions
void syncTimeWithNTP();
void updateRTCFromNTP();
void initializeRTC();
void checkNTPSync();
void getCurrentTime();
bool isDaylightSavingActive();

// Display functions
void displayTime();
void displayAlwaysOn();
void displayAMPM();
int getDisplayHour();
void displayHour_func(int hour);
void displayMinutes();

// Configuration mode functions
void enterConfigMode();
void setupWebServer();
String generateConfigPage();
String generateUpdateSection();
String generateUpdateJavaScript();

// Web handlers
void handleRoot();
void handleWiFiScan();
void handleSave();
void handleStatus();
void handleReset();
void handleRestart();
void handleConfigButton();

// OTA functions
int compareVersions(const String& version1, const String& version2);
bool checkForUpdates();
void performOTAUpdate();
bool downloadFirmware();
void checkForUpdatesIfNeeded();
void handleCheckUpdate();
void handlePerformUpdate();
void handleUpdateProgress();
void handleUpdateStatus();
void handleDownloadUpdate();

// ==================== UTILITY FUNCTIONS ====================
void lightUpWord(const int* leds_array, int count) {
    for (int i = 0; i < count; i++) {
        leds[leds_array[i]] = CRGB::White;
    }
}

void updateBrightness() {
    int brightnessReading = analogRead(BRIGHTNESS_PIN);
    int brightness = map(brightnessReading, 0, 4095, 10, 255);
    FastLED.setBrightness(brightness);
}

// ==================== ANIMATION FUNCTIONS ====================
void startupAnimation() {
    for (int i = 0; i < NUM_LEDS; i++) {
        leds[i] = CHSV(i * 255 / NUM_LEDS, 255, 255);
        FastLED.show();
        delay(50);
    }
    
    for (int brightness = 255; brightness >= 0; brightness -= 5) {
        FastLED.setBrightness(brightness);
        FastLED.show();
        delay(20);
    }
    
    FastLED.clear();
    FastLED.setBrightness(config.brightness);
    FastLED.show();
}

void configModeAnimation() {
    static unsigned long lastUpdate = 0;
    static int brightness = 0;
    static int direction = 1;
    
    if (millis() - lastUpdate > 50) {
        brightness += direction * 10;
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

void showUpdateAnimation() {
    Serial.println("Showing update start animation");
    
    for (int i = 0; i < 3; i++) {
        for (int brightness = 0; brightness < 255; brightness += 5) {
            fill_solid(leds, NUM_LEDS, CHSV(160, 255, brightness));
            FastLED.show();
            delay(10);
        }
        for (int brightness = 255; brightness > 0; brightness -= 5) {
            fill_solid(leds, NUM_LEDS, CHSV(160, 255, brightness));
            FastLED.show();
            delay(10);
        }
    }
    FastLED.clear();
    FastLED.show();
}

void showUpdateProgress(int progress) {
    FastLED.clear();
    
    int ledsToLight = (NUM_LEDS * progress) / 100;
    
    for (int i = 0; i < ledsToLight; i++) {
        leds[i] = CHSV(96, 255, 200);
    }
    
    if (ledsToLight < NUM_LEDS) {
        leds[ledsToLight] = CHSV(64, 255, 255);
    }
    
    FastLED.show();
}

void showUpdateSuccess() {
    Serial.println("Showing update success animation");
    
    for (int i = 0; i < 3; i++) {
        fill_solid(leds, NUM_LEDS, CRGB::Green);
        FastLED.show();
        delay(500);
        FastLED.clear();
        FastLED.show();
        delay(200);
    }
}

void showUpdateError() {
    Serial.println("Showing update error animation");
    
    for (int i = 0; i < 5; i++) {
        fill_solid(leds, NUM_LEDS, CRGB::Red);
        FastLED.show();
        delay(300);
        FastLED.clear();
        FastLED.show();
        delay(200);
    }
    
    updateInProgress = false;
    updateAvailable = false;
}

// ==================== VERSION COMPARISON ====================
int compareVersions(const String& version1, const String& version2) {
    int v1Major = 0, v1Minor = 0, v1Patch = 0;
    int v2Major = 0, v2Minor = 0, v2Patch = 0;
    
    sscanf(version1.c_str(), "%d.%d.%d", &v1Major, &v1Minor, &v1Patch);
    sscanf(version2.c_str(), "%d.%d.%d", &v2Major, &v2Minor, &v2Patch);
    
    if (v1Major != v2Major) return (v1Major > v2Major) ? 1 : -1;
    if (v1Minor != v2Minor) return (v1Minor > v2Minor) ? 1 : -1;
    if (v1Patch != v2Patch) return (v1Patch > v2Patch) ? 1 : -1;
    
    return 0;
}

// ==================== OTA UPDATE FUNCTIONS ====================
bool checkForUpdates() {
    if (!wifiConnected) {
        Serial.println("WiFi not connected, cannot check for updates");
        return false;
    }
    
    Serial.println("Checking for firmware updates...");
    
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    
    http.begin(client, GITHUB_API_URL);
    http.addHeader("User-Agent", "WordClock-OTA-Updater");
    
    int httpResponseCode = http.GET();
    
    if (httpResponseCode == 200) {
        String payload = http.getString();
        
        DynamicJsonDocument doc(2048);
        deserializeJson(doc, payload);
        
        latestVersion = doc["tag_name"].as<String>();
        
        if (latestVersion.startsWith("v")) {
            latestVersion = latestVersion.substring(1);
        }
        
        JsonArray assets = doc["assets"];
        for (JsonObject asset : assets) {
            String assetName = asset["name"].as<String>();
            if (assetName.endsWith(".uf2")) {
                downloadUrl = asset["browser_download_url"].as<String>();
                break;
            }
        }
        
        Serial.printf("Current version: %s, Latest version: %s\n", 
                     FIRMWARE_VERSION, latestVersion.c_str());
        
        if (compareVersions(latestVersion, FIRMWARE_VERSION) > 0 && !downloadUrl.isEmpty()) {
            updateAvailable = true;
            Serial.println("Update available!");
            return true;
        } else {
            updateAvailable = false;
            Serial.println("No update available");
        }
    } else {
        Serial.printf("Failed to check for updates. HTTP response: %d\n", httpResponseCode);
    }
    
    http.end();
    lastUpdateCheck = millis();
    return false;
}

void performOTAUpdate() {
    if (!updateAvailable || downloadUrl.isEmpty()) {
        Serial.println("No update available to install");
        return;
    }
    
    updateInProgress = true;
    updateProgress = 0;
    
    Serial.printf("Starting update to version %s\n", latestVersion.c_str());
    Serial.printf("Download URL: %s\n", downloadUrl.c_str());
    
    showUpdateAnimation();
    
    if (downloadFirmware()) {
        showUpdateSuccess();
        Serial.println("Update downloaded successfully!");
        Serial.println("Please follow the web instructions to complete the update.");
    } else {
        showUpdateError();
    }
    
    updateInProgress = false;
    updateProgress = 0;
}

bool downloadFirmware() {
    Serial.println("Downloading firmware...");
    
    if (!LittleFS.begin()) {
        Serial.println("Failed to initialize LittleFS");
        return false;
    }
    
    if (LittleFS.exists(UPDATE_FILE_PATH)) {
        LittleFS.remove(UPDATE_FILE_PATH);
    }
    
    File updateFile = LittleFS.open(UPDATE_FILE_PATH, "w");
    if (!updateFile) {
        Serial.println("Failed to create update file");
        return false;
    }
    
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    
    http.begin(client, downloadUrl);
    int httpResponseCode = http.GET();
    
    if (httpResponseCode == 200) {
        int contentLength = http.getSize();
        WiFiClient* stream = http.getStreamPtr();
        
        int downloaded = 0;
        uint8_t buffer[1024];
        
        while (http.connected() && downloaded < contentLength) {
            size_t available = stream->available();
            if (available > 0) {
                size_t readBytes = stream->readBytes(buffer, min(available, sizeof(buffer)));
                updateFile.write(buffer, readBytes);
                downloaded += readBytes;
                
                updateProgress = (downloaded * 100) / contentLength;
                showUpdateProgress(updateProgress);
                
                Serial.printf("Download progress: %d%%\r", updateProgress);
            }
            delay(1);
        }
        
        updateFile.close();
        http.end();
        
        if (downloaded == contentLength) {
            Serial.println("\nFirmware download complete!");
            return true;
        } else {
            Serial.println("\nIncomplete download!");
            LittleFS.remove(UPDATE_FILE_PATH);
            return false;
        }
    }
    
    updateFile.close();
    http.end();
    return false;
}

void checkForUpdatesIfNeeded() {
    if (!wifiConnected || updateInProgress) return;
    
    if (millis() - lastUpdateCheck > UPDATE_CHECK_INTERVAL) {
        checkForUpdates();
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
    Serial.println("Initializing EEPROM...");
    EEPROM.begin(EEPROM_SIZE);
    Serial.println("EEPROM initialized successfully");
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

// ==================== HARDWARE RESET FUNCTIONS ====================
void cleanupBeforeReset() {
    Serial.println("Cleaning up before reset...");
    
    server.stop();
    dnsServer.stop();
    
    if (timeClient != nullptr) {
        timeClient->end();
        delete timeClient;
        timeClient = nullptr;
    }
    
    ntpUDP.stop();
    
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    
    FastLED.clear();
    FastLED.show();
    
    delay(1000);
    
    Serial.println("Cleanup complete");
}

void performHardwareReset() {
    Serial.println("Performing hardware reset in 3 seconds...");
    Serial.flush();
    
    cleanupBeforeReset();
    
    delay(3000);
    
    watchdog_enable(1, 1);
    
    while (true) {
        tight_loop_contents();
    }
}

// ==================== WIFI FUNCTIONS ====================
bool connectToWiFi() {
    if (strlen(config.ssid) == 0) {
        Serial.println("No SSID configured");
        return false;
    }
    
    Serial.printf("Connecting to WiFi: %s\n", config.ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssid, config.password);
    
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < WIFI_TIMEOUT) {
        delay(500);
        Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.println("\nWiFi connected!");
        Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
        return true;
    } else {
        wifiConnected = false;
        Serial.println("\nWiFi connection failed!");
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
    Serial.printf("NTP client initialized with server: %s, base offset: %d\n", config.ntpServer, config.timezoneOffset);
}

void cleanupWiFi() {
    Serial.println("Cleaning up WiFi connections...");
    
    if (timeClient != nullptr) {
        timeClient->end();
        delete timeClient;
        timeClient = nullptr;
    }
    
    ntpUDP.stop();
    
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    
    delay(1000);
    
    wifiConnected = false;
    Serial.println("WiFi cleanup complete");
}

// ==================== TIME FUNCTIONS ====================
void syncTimeWithNTP() {
    if (!wifiConnected || timeClient == nullptr) return;
    
    Serial.println("Syncing with NTP server...");
    
    int attempts = 0;
    while (!timeClient->update() && attempts < 10) {
        timeClient->forceUpdate();
        delay(1000);
        attempts++;
        Serial.print(".");
    }
    
    if (attempts < 10) {
        lastNTPSync = millis();
        updateRTCFromNTP();
        Serial.println("\nNTP sync successful!");
    } else {
        Serial.println("\nNTP sync failed!");
    }
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
    
    Serial.printf("RTC updated: %02d:%02d:%02d %02d/%02d/%04d %s\n", 
                  currentTime.hour, currentTime.min, currentTime.sec,
                  currentTime.day, currentTime.month, currentTime.year,
                  isDaylightSavingActive() ? "(DST)" : "(STD)");
}

void initializeRTC() {
    rtc_init();
    Serial.println("Internal RTC initialized");
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
    for (int i = 0; i < 3; i++) {
        leds[HET_LEDS[i]] = CRGB::Yellow;
    }
    for (int i = 0; i < 5; i++) {
        leds[IS_LEDS[i]] = CRGB::Yellow;
    }
}

void displayAMPM() {
    if (currentTime.hour < 12) {
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

void displayMinutes() {
    int minute = currentTime.min;
    
    if (minute == 0) {
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
        lightUpWord(BIJNA_LEDS, 4);
    }
}

// ==================== CONFIGURATION MODE ====================
void enterConfigMode() {
    cleanupWiFi();
    
    configMode = true;
    
    Serial.println("Starting Access Point...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    
    IPAddress IP = WiFi.softAPIP();
    Serial.printf("AP IP address: %s\n", IP.toString().c_str());
    
    dnsServer.start(DNS_PORT, "*", IP);
    setupWebServer();
    server.begin();
    
    Serial.println("Web server started - Connect to 'WordClock-Setup' WiFi network");
}

void setupWebServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/scan", HTTP_GET, handleWiFiScan);
    server.on("/reset", HTTP_POST, handleReset);
    server.on("/restart", HTTP_POST, handleRestart);
    server.on("/check-update", HTTP_GET, handleCheckUpdate);
    server.on("/perform-update", HTTP_GET, handlePerformUpdate);
    server.on("/update-progress", HTTP_GET, handleUpdateProgress);
    server.on("/update-status", HTTP_GET, handleUpdateStatus);
    server.on("/download-update", HTTP_GET, handleDownloadUpdate);
    server.onNotFound(handleRoot);
}

String generateUpdateSection() {
    String html = "<div style='margin-top:20px;padding:15px;background:#f9f9f9;border-radius:5px;'>";
    html += "<h3>🔄 Firmware Update</h3>";
    html += "<div class='form-group'>";
    html += "<label>Current Version:</label>";
    html += "<div style='padding:5px;background:white;border:1px solid #ddd;border-radius:3px;'>";
    html += FIRMWARE_VERSION;
    html += "</div></div>";
    
    if (updateAvailable) {
        html += "<div class='form-group'>";
        html += "<label>Available Version:</label>";
        html += "<div style='padding:5px;background:#e8f5e8;border:1px solid #4CAF50;border-radius:3px;color:#2e7d2e;'>";
        html += latestVersion;
        html += " (Update Available!)</div></div>";
        
        html += "<button type='button' onclick='performUpdate()' style='background:#ff9800;color:white;padding:10px 20px;border:none;border-radius:4px;margin:5px;'>📥 Download Update</button>";
    } else {
        html += "<div class='form-group' style='color:#666;'>No updates available</div>";
    }
    
    if (LittleFS.exists(UPDATE_FILE_PATH)) {
        html += "<div style='background:#e8f5e8;padding:10px;border-radius:5px;margin:10px 0;'>";
        html += "<strong>✅ Update Ready!</strong><br>";
        html += "<a href='/update-status'>Click here for installation instructions</a>";
        html += "</div>";
    }
    
    html += "<button type='button' onclick='checkUpdates()' style='background:#2196F3;color:white;padding:10px 20px;border:none;border-radius:4px;'>🔍 Check for Updates</button>";
    html += "</div>";
    
    return html;
}

String generateUpdateJavaScript() {
    String script = "";
    script += "function checkUpdates() {";
    script += "  fetch('/check-update').then(r=>r.text()).then(data=>{";
    script += "    if(data.includes('available')) { location.reload(); }";
    script += "    else { alert('No updates available'); }";
    script += "  }).catch(e=>alert('Update check failed'));";
    script += "}";
    
    script += "function performUpdate() {";
    script += "  if(!confirm('This will download the firmware update. Continue?')) return;";
    script += "  window.location.href = '/perform-update';";
    script += "}";
    
    return script;
}

String generateConfigPage() {
    String html = "<!DOCTYPE html><html><head>";
    html += "<title>Word Clock Setup</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "body{font-family:Arial;margin:20px;background:#f0f0f0;}";
    html += ".container{max-width:600px;margin:0 auto;background:white;padding:20px;border-radius:10px;}";
    html += "h1{color:#333;text-align:center;}";
    html += ".form-group{margin:15px 0;}";
    html += "label{display:block;margin-bottom:5px;font-weight:bold;}";
    html += "input,select{width:100%;padding:8px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;}";
    html += "button{background:#4CAF50;color:white;padding:12px 20px;border:none;border-radius:4px;cursor:pointer;margin:5px;}";
    html += "button:hover{background:#45a049;}";
    html += ".danger{background:#f44336;}";
    html += ".warning{background:#ff9800;}";
    html += ".wifi-list{max-height:150px;overflow-y:auto;border:1px solid #ddd;padding:10px;}";
    html += ".wifi-item{cursor:pointer;padding:5px;border-bottom:1px solid #eee;}";
    html += "</style></head><body>";
    
    html += "<div class='container'>";
    html += "<h1>Word Clock Setup</h1>";
    
    html += "<button onclick='scanWiFi()'>Scan WiFi</button>";
    html += "<div id='wifi-results' class='wifi-list' style='display:none;'></div>";
    
    html += "<form action='/save' method='post'>";
    html += "<div class='form-group'>";
    html += "<label>WiFi Network:</label>";
    html += "<input type='text' name='ssid' value='" + String(config.ssid) + "' required>";
    html += "</div>";
    
    html += "<div class='form-group'>";
    html += "<label>WiFi Password:</label>";
    html += "<input type='password' name='password' value='" + String(config.password) + "'>";
    html += "</div>";
    
    html += "<div class='form-group'>";
    html += "<label>Timezone:</label>";
    html += "<select name='timezone'>";
    html += "<option value='0'"; if (config.timezoneOffset == 0) html += " selected"; html += ">UTC+0 (London)</option>";
    html += "<option value='3600'"; if (config.timezoneOffset == 3600) html += " selected"; html += ">UTC+1 (Amsterdam/Berlin/Paris)</option>";
    html += "<option value='7200'"; if (config.timezoneOffset == 7200) html += " selected"; html += ">UTC+2 (Athens/Helsinki)</option>";
    html += "<option value='-18000'"; if (config.timezoneOffset == -18000) html += " selected"; html += ">UTC-5 (New York)</option>";
    html += "<option value='-21600'"; if (config.timezoneOffset == -21600) html += " selected"; html += ">UTC-6 (Chicago)</option>";
    html += "<option value='-25200'"; if (config.timezoneOffset == -25200) html += " selected"; html += ">UTC-7 (Denver)</option>";
    html += "<option value='-28800'"; if (config.timezoneOffset == -28800) html += " selected"; html += ">UTC-8 (Los Angeles)</option>";
    html += "</select></div>";
    
    html += "<div class='form-group'>";
    html += "<label><input type='checkbox' name='daylight_saving' value='1'";
    if (config.daylightSaving) html += " checked";
    html += "> Enable Daylight Saving Time (EU rules)</label>";
    html += "</div>";
    
    html += "<div class='form-group'>";
    html += "<label>NTP Server:</label>";
    html += "<input type='text' name='ntpserver' value='" + String(config.ntpServer) + "'>";
    html += "</div>";
    
    html += "<div class='form-group'>";
    html += "<label>Brightness (10-255):</label>";
    html += "<input type='number' name='brightness' min='10' max='255' value='" + String(config.brightness) + "'>";
    html += "</div>";
    
    html += "<button type='submit'>Save & Restart</button>";
    html += "<button type='button' onclick=\"location.href='/status'\">Status</button>";
    html += "</form>";
    
    html += generateUpdateSection();
    
    html += "<form action='/restart' method='post' style='margin-top:20px;'>";
    html += "<button type='submit' class='warning' onclick=\"return confirm('Restart system?')\">Restart System</button>";
    html += "</form>";
    
    html += "<form action='/reset' method='post'>";
    html += "<button type='submit' class='danger' onclick=\"return confirm('Reset all settings?')\">Factory Reset</button>";
    html += "</form>";
    html += "</div>";
    
    html += "<script>";
    html += "function scanWiFi(){fetch('/scan').then(r=>r.text()).then(data=>{document.getElementById('wifi-results').innerHTML=data;document.getElementById('wifi-results').style.display='block';});}";
    html += "function selectWiFi(ssid){document.querySelector('input[name=ssid]').value=ssid;}";
    html += generateUpdateJavaScript();
    html += "</script>";
    html += "</body></html>";
    
    return html;
}

// ==================== WEB HANDLERS ====================
void handleRoot() {
    String html = generateConfigPage();
    server.send(200, "text/html", html);
}

void handleWiFiScan() {
    String html = "";
    int n = WiFi.scanNetworks();
    
    if (n == 0) {
        html = "<div>No networks found</div>";
    } else {
        for (int i = 0; i < n; i++) {
            String ssid = WiFi.SSID(i);
            String rssi = String(WiFi.RSSI(i));
            html += "<div class='wifi-item' onclick='selectWiFi(\"" + ssid + "\")'>";
            html += ssid + " (" + rssi + "dBm)</div>";
        }
    }
    
    server.send(200, "text/html", html);
}

void handleSave() {
    strcpy(config.ssid, server.arg("ssid").c_str());
    strcpy(config.password, server.arg("password").c_str());
    strcpy(config.ntpServer, server.arg("ntpserver").c_str());
    config.timezoneOffset = server.arg("timezone").toInt();
    config.brightness = server.arg("brightness").toInt();
    config.daylightSaving = server.hasArg("daylight_saving") && server.arg("daylight_saving") == "1";
    
    saveConfiguration();
    
    String html = "<!DOCTYPE html><html><head><title>Saved</title>";
    html += "<style>body{font-family:Arial;text-align:center;margin:50px;}</style>";
    html += "</head><body><h1>Configuration Saved!</h1>";
    html += "<p>System will restart in 3 seconds...</p>";
    html += "<p>Please reconnect to your regular WiFi network after restart.</p>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
    delay(1000);
    
    performHardwareReset();
}

void handleStatus() {
    String status = wifiConnected ? "Connected" : "Disconnected";
    String ip = wifiConnected ? WiFi.localIP().toString() : "None";
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<title>Word Clock Status</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<meta http-equiv='refresh' content='10'>";
    html += "<style>body{font-family:Arial;margin:20px;background:#f0f0f0;}";
    html += ".container{max-width:600px;margin:0 auto;background:white;padding:20px;border-radius:10px;}";
    html += ".item{margin:10px 0;padding:10px;background:#f9f9f9;border-radius:5px;}";
    html += "button{background:#4CAF50;color:white;padding:10px 20px;border:none;border-radius:4px;}";
    html += "</style></head><body>";
    
    html += "<div class='container'>";
    html += "<h1>Word Clock Status</h1>";
    html += "<div class='item'>Firmware: " + String(FIRMWARE_VERSION) + " (Pico W)</div>";
    html += "<div class='item'>WiFi: " + status + "</div>";
    html += "<div class='item'>IP: " + ip + "</div>";
    html += "<div class='item'>SSID: " + String(config.ssid) + "</div>";
    html += "<div class='item'>Time: " + String(currentTime.hour) + ":" + 
             (currentTime.min < 10 ? "0" : "") + String(currentTime.min) + 
             (isDaylightSavingActive() ? " (DST)" : " (STD)") + "</div>";
    
    if (updateAvailable) {
        html += "<div class='item' style='background:#e8f5e8;color:#2e7d2e;'>Update Available: " + latestVersion + "</div>";
    }
    
    if (LittleFS.exists(UPDATE_FILE_PATH)) {
        html += "<div class='item' style='background:#fff3cd;border-color:#ffeaa7;color:#856404;'>";
        html += "Update Downloaded - <a href='/update-status'>Installation Instructions</a>";
        html += "</div>";
    }
    
    html += "<button onclick=\"location.href='/'\">Back</button>";
    html += "</div></body></html>";
    
    server.send(200, "text/html", html);
}

void handleReset() {
    resetConfiguration();
    
    String html = "<!DOCTYPE html><html><head><title>Reset</title>";
    html += "<style>body{font-family:Arial;text-align:center;margin:50px;}</style>";
    html += "</head><body><h1>Factory Reset Complete</h1>";
    html += "<p>System will restart in 3 seconds...</p></body></html>";
    
    server.send(200, "text/html", html);
    delay(1000);
    
    Serial.println("Factory reset performed");
    performHardwareReset();
}

void handleRestart() {
    String html = "<!DOCTYPE html><html><head><title>Restarting</title>";
    html += "<style>body{font-family:Arial;text-align:center;margin:50px;}</style>";
    html += "</head><body><h1>System Restarting</h1>";
    html += "<p>Please wait for restart to complete...</p></body></html>";
    
    server.send(200, "text/html", html);
    delay(1000);
    
    Serial.println("Manual restart requested");
    performHardwareReset();
}

void handleConfigButton() {
    bool currentButtonState = digitalRead(CONFIG_BUTTON_PIN) == LOW;
    
    if (currentButtonState && !buttonPressed) {
        buttonPressed = true;
        buttonPressStart = millis();
    } else if (!currentButtonState && buttonPressed) {
        buttonPressed = false;
        unsigned long pressTime = millis() - buttonPressStart;
        if (pressTime >= BUTTON_HOLD_TIME) {
            Serial.println("Config button held - entering configuration mode");
            enterConfigMode();
        }
    }
}

// ==================== OTA WEB HANDLERS ====================
void handleCheckUpdate() {
    Serial.println("Manual update check requested via web interface");
    
    bool available = checkForUpdates();
    
    String response = "Current version: " + String(FIRMWARE_VERSION);
    if (available) {
        response += "\nUpdate available: " + latestVersion;
    } else {
        response += "\nNo updates available";
    }
    
    server.send(200, "text/plain", response);
}

void handlePerformUpdate() {
    Serial.println("Manual update requested via web interface");
    
    if (!updateAvailable) {
        server.send(400, "text/plain", "No update available");
        return;
    }
    
    updateInProgress = true;
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<title>Updating Pico W</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<meta http-equiv='refresh' content='5;url=/update-status'>";
    html += "<style>";
    html += "body{font-family:Arial;text-align:center;margin:50px;background:#f0f0f0;}";
    html += ".container{max-width:500px;margin:0 auto;background:white;padding:20px;border-radius:10px;}";
    html += ".progress{width:100%;background:#ddd;border-radius:10px;margin:20px 0;}";
    html += ".progress-bar{height:30px;background:#4CAF50;border-radius:10px;width:0%;transition:width 0.3s;}";
    html += "</style></head><body>";
    
    html += "<div class='container'>";
    html += "<h1>🔄 Firmware Update In Progress</h1>";
    html += "<p>Downloading firmware version " + latestVersion + "...</p>";
    html += "<div class='progress'><div class='progress-bar' id='progress-bar'></div></div>";
    html += "<p><strong>⚠️ Do NOT power off the device!</strong></p>";
    html += "<p>This page will refresh automatically to show progress.</p>";
    html += "</div>";
    
    html += "<script>";
    html += "setInterval(function() {";
    html += "  fetch('/update-progress').then(r=>r.text()).then(progress=>{";
    html += "    document.getElementById('progress-bar').style.width=progress+'%';";
    html += "  });";
    html += "}, 1000);";
    html += "</script>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
    
    delay(1000);
    performOTAUpdate();
}

void handleUpdateProgress() {
    server.send(200, "text/plain", String(updateProgress));
}

void handleUpdateStatus() {
    String html = "<!DOCTYPE html><html><head>";
    html += "<title>Update Status</title>";
    html += "<style>";
    html += "body{font-family:Arial;text-align:center;margin:50px;background:#f0f0f0;}";
    html += ".container{max-width:600px;margin:0 auto;background:white;padding:20px;border-radius:10px;}";
    html += ".step{margin:20px 0;padding:15px;background:#f9f9f9;border-radius:5px;text-align:left;}";
    html += ".highlight{background:#e8f5e8;border:2px solid #4CAF50;}";
    html += "</style></head><body>";
    
    html += "<div class='container'>";
    
    if (updateInProgress) {
        html += "<h1>⏳ Download In Progress</h1>";
        html += "<p>Progress: " + String(updateProgress) + "%</p>";
        html += "<p>Please wait...</p>";
    } else if (LittleFS.exists(UPDATE_FILE_PATH)) {
        html += "<h1>✅ Download Complete!</h1>";
        html += "<h2>Manual Update Required</h2>";
        html += "<p>The firmware has been downloaded. Follow these steps:</p>";
        
        html += "<div class='step highlight'>";
        html += "<h3>Step 1: Download Update File</h3>";
        html += "<p><a href='/download-update' download='wordclock-update.uf2'>";
        html += "<button style='padding:10px 20px;font-size:16px;background:#4CAF50;color:white;border:none;border-radius:5px;'>📥 Download UF2 File</button></a></p>";
        html += "</div>";
        
        html += "<div class='step'>";
        html += "<h3>Step 2: Enter BOOTSEL Mode</h3>";
        html += "<p>1. Unplug your Pico W from power</p>";
        html += "<p>2. Hold down the BOOTSEL button on your Pico W</p>";
        html += "<p>3. While holding BOOTSEL, plug the USB cable back in</p>";
        html += "<p>4. Release the BOOTSEL button</p>";
        html += "<p>5. Your computer should show a drive called 'RPI-RP2'</p>";
        html += "</div>";
        
        html += "<div class='step'>";
        html += "<h3>Step 3: Install Update</h3>";
        html += "<p>1. Drag the downloaded .uf2 file to the 'RPI-RP2' drive</p>";
        html += "<p>2. The Pico W will automatically restart with new firmware</p>";
        html += "<p>3. Wait for the startup animation</p>";
        html += "</div>";
        
        html += "<p><strong>Version:</strong> " + latestVersion + "</p>";
    } else {
        html += "<h1>❌ Download Failed</h1>";
        html += "<p>Please try again or check your internet connection.</p>";
        html += "<button onclick=\"location.href='/'\">Back to Config</button>";
    }
    
    html += "</div></body></html>";
    
    server.send(200, "text/html", html);
}

void handleDownloadUpdate() {
    if (!LittleFS.exists(UPDATE_FILE_PATH)) {
        server.send(404, "text/plain", "Update file not found");
        return;
    }
    
    File updateFile = LittleFS.open(UPDATE_FILE_PATH, "r");
    if (!updateFile) {
        server.send(500, "text/plain", "Cannot open update file");
        return;
    }
    
    String filename = "wordclock-update-" + latestVersion + ".uf2";
    
    server.sendHeader("Content-Type", "application/octet-stream");
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
    
    server.streamFile(updateFile, "application/octet-stream");
    updateFile.close();
    
    Serial.println("Update file downloaded by user");
}

// ==================== SETUP FUNCTION ====================
void setup() {
    Serial.begin(9600);
    Serial.println("Starting Dutch Word Clock with Web Configuration...");
    
    // Initialize LittleFS voor update bestanden
    if (!LittleFS.begin()) {
        Serial.println("LittleFS initialization failed");
    } else {
        Serial.println("LittleFS initialized successfully");
    }
    
    setupEEPROM();
    
    pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
    analogReadResolution(12);
    
    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
    FastLED.setBrightness(BRIGHTNESS);
    
    loadConfiguration();
    initializeRTC();
    startupAnimation();
    
    if (!config.configured) {
        Serial.println("No configuration found, entering setup mode...");
        enterConfigMode();
    } else {
        if (connectToWiFi()) {
            initializeNTPClient();
            syncTimeWithNTP();
        } else {
            Serial.println("WiFi connection failed, entering config mode...");
            enterConfigMode();
        }
    }
    
    Serial.println("Setup complete!");
}

// ==================== MAIN LOOP ====================
void loop() {
    handleConfigButton();
    
    if (configMode) {
        dnsServer.processNextRequest();
        server.handleClient();
        configModeAnimation();
    } else {
        updateBrightness();
        
        if (wifiConnected && timeClient != nullptr) {
            checkNTPSync();
        }
        
        if (wifiConnected) {
            checkForUpdatesIfNeeded();
        }
        
        getCurrentTime();
        displayTime();
        FastLED.show();
        delay(1000);
    }
    
    delay(50);
}
