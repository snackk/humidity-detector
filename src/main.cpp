#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

extern "C" {
  #include "user_interface.h"
}

// WiFi credentials
const char* ssid = "WIFI";
const char* password = "PASS";

// WhatsApp CallMeBot API configuration
String phoneNumber = "+3NUM";
String apiKey = "XXX";

// Circuit breaker monitoring
#define LED_PIN 2              // Built-in LED

// Soil moisture
#define SOIL_PIN A0            // YL-69 analog output on A0

const int ADC_DRY = 1024;    
const int ADC_WET = 423;  

// WiFi connection handling
#define MAX_WIFI_ATTEMPTS 5
#define WIFI_TIMEOUT 15000
#define RECONNECT_DELAY 5000

int connectionAttempts = 0;
unsigned long wifiConnectStartTime = 0;
bool shouldReconnect = false;
unsigned long lastDisconnectTime = 0;

WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;

// NTP Time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

// Moisture sampling timing (15 minutes)
const unsigned long MOISTURE_INTERVAL = 15UL * 60UL * 1000UL;
unsigned long lastMoistureSample = 0;

// Function prototypes
void initWiFi();
void connectToWiFi();
void handleWiFiReconnection();
String getBestBSSID();
void convertBSSIDStringToBytes(const String& bssidStr, uint8_t* bssidBytes);
void onWifiConnect(const WiFiEventStationModeGotIP& event);
void onWifiDisconnect(const WiFiEventStationModeDisconnected& event);
void sendWhatsAppMessage(String message);
String urlEncode(String str);
void handleTripEvent();
void handleRestoreEvent();
String getFormattedTime();
String formatDowntime(unsigned long seconds);
void sampleAndReportMoisture();

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n=== Monitor Bomba + Humidade Solo ===");

  // Configure pins
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  pinMode(SOIL_PIN, INPUT);

  // Initialize WiFi
  initWiFi();

  // Wait for WiFi connection before continuing
  Serial.println("Aguardando ligação WiFi...");
  int waitCount = 0;
  while (WiFi.status() != WL_CONNECTED && waitCount < 30) {
    delay(1000);
    Serial.print(".");
    waitCount++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    // Initialize NTP client
    timeClient.begin();
    timeClient.setTimeOffset(0);

    Serial.println("A sincronizar hora...");
    for (int i = 0; i < 3; i++) {
      if (timeClient.update()) {
        Serial.println("Hora sincronizada!");
        break;
      }
      delay(1000);
    }

    // First soil measurement right away
    sampleAndReportMoisture();
    lastMoistureSample = millis();
  }
}

void loop() {
  delay(1000);
}

// ---------- Soil moisture ----------

void sampleAndReportMoisture() {
  int raw = analogRead(SOIL_PIN);
  Serial.print("Leitura humidade solo (raw): ");
  Serial.println(raw);

  int moisturePercent = map(raw, ADC_DRY, ADC_WET, 0, 100);
  moisturePercent = constrain(moisturePercent, 0, 100);

  Serial.print("Humidade solo estimada: ");
  Serial.print(moisturePercent);
  Serial.println("%");

  String msg = "💧 Medição humidade solo\n";
  msg += "⏰ " + getFormattedTime() + "\n";
  msg += "📊 Valor bruto: " + String(raw) + "\n";
  msg += "📈 Estimativa: " + String(moisturePercent) + "%";

  // Try to send for 3 times
  bool sent = false;
  for(int i = 0; i < 3 && !sent; i++) {
    sendWhatsAppMessage(msg);
    delay(2000);
    if(WiFi.status() == WL_CONNECTED) sent = true;
  }

  Serial.println("Deepsleep for 1H");
  Serial.flush();
  
  // Deep sleep por 1 hora (3600 segundos)
  digitalWrite(LED_PIN, LOW);
  ESP.deepSleep(3600 * 1000000LL);
}

// ---------- WiFi management ----------

void initWiFi() {
  Serial.println("Initializing WiFi...");

  // Configure WiFi for better reliability
  wifi_set_phy_mode(PHY_MODE_11G);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setOutputPower(20.5);

  // Setup event handlers
  wifiConnectHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& event) {
    onWifiConnect(event);
  });

  wifiDisconnectHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& event) {
    onWifiDisconnect(event);
  });

  // Start connection
  connectToWiFi();
}

void onWifiConnect(const WiFiEventStationModeGotIP& event) {
  connectionAttempts = 0;
  wifiConnectStartTime = 0;
  shouldReconnect = false;
  lastDisconnectTime = 0;

  Serial.println("\n=== WiFi Connected Successfully ===");
  Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
  Serial.printf("Signal Strength: %d dBm\n", WiFi.RSSI());
  Serial.printf("Channel: %d\n", WiFi.channel());
  Serial.printf("BSSID: %s\n", WiFi.BSSIDstr().c_str());
  Serial.println("===================================\n");
}

void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
  Serial.printf("\n=== WiFi Disconnected ===\n");
  Serial.printf("Reason Code: %d\n", event.reason);

  if (connectionAttempts < MAX_WIFI_ATTEMPTS) {
    shouldReconnect = true;
    lastDisconnectTime = millis();
    Serial.println("Will attempt reconnection...");
  } else {
    Serial.println("Max connection attempts reached!");
  }
  Serial.println("========================\n");
}

String getBestBSSID() {
  int n = WiFi.scanNetworks();
  String bestBSSID = "";
  int bestRSSI = -100;

  Serial.printf("Scanning for network: %s\n", ssid);

  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssid) {
      int currentRSSI = WiFi.RSSI(i);
      Serial.printf("Found %s: BSSID=%s, RSSI=%d dBm, Channel=%d\n",
                    ssid, WiFi.BSSIDstr(i).c_str(),
                    currentRSSI, WiFi.channel(i));

      if (currentRSSI > bestRSSI) {
        bestRSSI = currentRSSI;
        bestBSSID = WiFi.BSSIDstr(i);
      }
    }
  }

  if (bestBSSID != "") {
    Serial.printf("Selected best AP: BSSID=%s, RSSI=%d dBm\n",
                  bestBSSID.c_str(), bestRSSI);
  } else {
    Serial.println("ERROR: Target network not found!");
  }

  return bestBSSID;
}

void convertBSSIDStringToBytes(const String& bssidStr, uint8_t* bssidBytes) {
  int byteIndex = 0;
  size_t start = 0;
  size_t length = bssidStr.length();

  for (size_t i = 0; i <= length && byteIndex < 6; i++) {
    if (i == length || bssidStr.charAt(i) == ':') {
      if (i > start) {
        String hexPart = bssidStr.substring(start, i);
        bssidBytes[byteIndex] = (uint8_t)strtol(hexPart.c_str(), NULL, 16);
        byteIndex++;
      }
      start = i + 1;
    }
  }
}

void connectToWiFi() {
  Serial.printf("\n=== Connection Attempt #%d ===\n", connectionAttempts + 1);

  String bestBSSID = getBestBSSID();

  if (bestBSSID == "") {
    Serial.println("ERROR: Network not found!");
    connectionAttempts++;
    return;
  }

  // Get target channel
  int targetChannel = 0;
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    if (WiFi.BSSIDstr(i) == bestBSSID) {
      targetChannel = WiFi.channel(i);
      break;
    }
  }

  uint8_t bssidBytes[6];
  convertBSSIDStringToBytes(bestBSSID, bssidBytes);

  Serial.printf("Connecting to: %s\n", ssid);
  Serial.printf("Target BSSID: %s\n", bestBSSID.c_str());
  Serial.printf("Target Channel: %d\n", targetChannel);

  WiFi.begin(ssid, password, targetChannel, bssidBytes, true);
  delay(500);

  wifiConnectStartTime = millis();
  connectionAttempts++;

  Serial.println("Connection initiated...");
}

void handleWiFiReconnection() {
  // Handle connection timeout
  if (WiFi.status() != WL_CONNECTED &&
      wifiConnectStartTime > 0 &&
      millis() - wifiConnectStartTime > WIFI_TIMEOUT) {

    wl_status_t status = WiFi.status();
    if (status == WL_DISCONNECTED || status == WL_IDLE_STATUS || status == WL_NO_SSID_AVAIL) {
      Serial.println("WiFi connection timeout detected");
      wifiConnectStartTime = 0;

      if (connectionAttempts < MAX_WIFI_ATTEMPTS) {
        Serial.println("Retrying connection...");
        connectToWiFi();
      } else {
        Serial.println("Max attempts reached!");
        connectionAttempts = 0;  // Reset for future attempts
        delay(30000);            // Wait 30s before trying again
      }
    }
    return;
  }

  // Handle scheduled reconnection
  if (shouldReconnect && millis() - lastDisconnectTime > RECONNECT_DELAY) {
    shouldReconnect = false;

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Initiating scheduled reconnection...");
      connectToWiFi();
    }
  }
}

// ---------- Application functions ----------

String formatDowntime(unsigned long seconds) {
  if (seconds < 60) {
    return String(seconds) + " seg";
  } else if (seconds < 3600) {
    return String(seconds / 60) + " min";
  } else {
    unsigned long hours = seconds / 3600;
    unsigned long mins = (seconds % 3600) / 60;
    return String(hours) + "h " + String(mins) + "m";
  }
}

String getFormattedTime() {
  unsigned long epochTime = timeClient.getEpochTime();
  struct tm *ptm = gmtime((time_t *)&epochTime);

  String formattedTime = "";

  if (ptm->tm_mday < 10) formattedTime += "0";
  formattedTime += String(ptm->tm_mday) + "/";

  if (ptm->tm_mon + 1 < 10) formattedTime += "0";
  formattedTime += String(ptm->tm_mon + 1) + "/";
  formattedTime += String(ptm->tm_year + 1900) + " ";

  if (ptm->tm_hour < 10) formattedTime += "0";
  formattedTime += String(ptm->tm_hour) + ":";

  if (ptm->tm_min < 10) formattedTime += "0";
  formattedTime += String(ptm->tm_min) + ":";

  if (ptm->tm_sec < 10) formattedTime += "0";
  formattedTime += String(ptm->tm_sec);

  return formattedTime;
}

// ---------- WhatsApp helper ----------

void sendWhatsAppMessage(String message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi desligado! Não enviei mensagem.");
    return;
  }

  String url = "http://api.callmebot.com/whatsapp.php?phone=" + phoneNumber;
  url += "&text=" + urlEncode(message);
  url += "&apikey=" + apiKey;

  WiFiClient client;
  HTTPClient http;

  Serial.println("A enviar WhatsApp...");

  http.begin(client, url);
  http.setTimeout(10000);

  int httpResponseCode = http.GET();

  if (httpResponseCode == 200) {
    Serial.println("✓ Mensagem enviada!");
  } else {
    Serial.print("✗ Erro HTTP: ");
    Serial.println(httpResponseCode);
    if (httpResponseCode > 0) {
      Serial.println("Resposta: " + http.getString());
    }
  }

  http.end();
}

String urlEncode(String str) {
  String encodedString = "";
  char c;
  char code0;
  char code1;

  for (unsigned int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encodedString += '+';
    } else if (isalnum(c)) {
      encodedString += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) {
        code1 = (c & 0xf) - 10 + 'A';
      }
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) {
        code0 = c - 10 + 'A';
      }
      encodedString += '%';
      encodedString += code0;
      encodedString += code1;
    }
  }

  return encodedString;
}
