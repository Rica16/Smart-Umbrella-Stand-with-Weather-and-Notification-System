#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <Adafruit_PN532.h>

// --- NFC via SPI ---
#define SCK 18
#define MISO 19
#define MOSI 23
#define SS 5
Adafruit_PN532 nfc(SS);

// --- Pins ---
#define BUZZER_PIN 14
#define RED_LED 26
#define GREEN_LED 27
#define OBSTACLE_PIN 25
#define BUTTON_PIN 33

bool lastObstacleState = HIGH;

// --- Wi-Fi Credentials ---
const char* ssid = "Kae";
const char* password = "xgg69mysvy560";

// --- MQTT Settings ---
const char* mqttServer = "broker.hivemq.com";
const int mqttPort = 1883;
const char* mqttTopicWeather = "esp32/weather/alerts";
const char* mqttTopicWeatherNotification = "esp32/weather/alerts/notification";
const char* mqttTopicUmbrella = "esp32/umbrella/detected";
const char* mqttTopicSystem = "esp32/system/alerts";
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// --- Weather API Settings ---
const char* apiKey = "cced48f74baf450c7d1746b27a2280e8";
const char* latitude = "10.8000";
const char* longitude = "122.0167";
const char* weatherHost = "http://api.openweathermap.org/data/2.5/weather";

// --- Umbrella Data ---
#define MAX_UMBRELLAS 10
struct UmbrellaTag {
  uint8_t uid[7];
  uint8_t uidLength;
  bool isOnStand;
};
UmbrellaTag registeredUmbrellas[MAX_UMBRELLAS];
uint8_t umbrellaCount = 0;

bool umbrellaDetected = false;
bool systemOn = false;

// --- Timers and Flags ---
unsigned long lastWeatherSentTime = 0;
const unsigned long weatherInterval = 30 * 60 * 1000;
unsigned long lastDetectionTime = 0;
unsigned long lastScanTime = 0;
const unsigned long scanCooldown = 2000;
const unsigned long detectionTimeout = 2 * 60 * 1000;
bool rainExpectedWithoutUmbrella = false;
unsigned long rainWarningStartTime = 0;
bool secondRainWarningSent = false;

// --- Button State ---
bool lastButtonState = HIGH;
unsigned long buttonDownTime = 0;
unsigned long lastButtonClickTime = 0;
const unsigned long debounceDelay = 50;
const unsigned long longPressDuration = 1500;
const unsigned long doubleClickGap = 500;

// --- Setup ---
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(OBSTACLE_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  systemOn = false;
  digitalWrite(RED_LED, LOW);
  digitalWrite(GREEN_LED, LOW);

  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to Wi-Fi");

  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setKeepAlive(60);
  connectToMQTT();

  nfc.begin();
  if (!nfc.getFirmwareVersion()) {
    Serial.println("PN532 not found");
    while (1);
  }
  nfc.SAMConfig();
  fetchWeather();
  lastWeatherSentTime = millis();
}

void loop() {
  handleButtonToggle();

  if (!systemOn) {
    delay(100);
    return;
  }

  if (!mqttClient.connected()) connectToMQTT();
  mqttClient.loop();

  handleObstacleSensor();

  uint8_t uid[7];
  uint8_t uidLength;
  umbrellaDetected = false;
  bool physicalUmbrellaDetected = digitalRead(OBSTACLE_PIN) == LOW;

  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
    if (millis() - lastScanTime < scanCooldown) return;
    lastScanTime = millis();

    int idx = findRegisteredTagIndex(uid, uidLength);
    String uidStr = "";
    for (uint8_t i = 0; i < uidLength; i++) uidStr += String(uid[i], HEX);

    if (idx == -1) {
      registerNewUmbrella(uid, uidLength);
      triggerNormalAlert(uidStr);
    } else if (!registeredUmbrellas[idx].isOnStand) {
      registeredUmbrellas[idx].isOnStand = true;
      mqttClient.publish(mqttTopicUmbrella, "✅ Umbrella placed on stand.");
      Serial.println("Umbrella placed on stand (via NFC).");
      mqttClient.publish(mqttTopicUmbrella, "☂️ Umbrella placed back on stand.");
      Serial.println("Notification: Umbrella placed back on stand.");
      triggerNormalAlert(uidStr);
    } else {
      mqttClient.publish(mqttTopicUmbrella, "ℹ️ Umbrella already on stand.");
      Serial.println("Notification: Umbrella already on stand.");
      triggerDuplicateScanAlert();
    }

    umbrellaDetected = true;
    lastDetectionTime = millis();
  } else if (physicalUmbrellaDetected) {
    for (int i = 0; i < umbrellaCount; i++) {
      if (!registeredUmbrellas[i].isOnStand) {
        registeredUmbrellas[i].isOnStand = true;
        mqttClient.publish(mqttTopicUmbrella, "✅ Umbrella placed (obstacle).");
        Serial.println("Umbrella physically placed detected via obstacle sensor.");
        mqttClient.publish(mqttTopicUmbrella, "☂️ Umbrella physically placed.");
        Serial.println("Notification: Umbrella physically placed.");
        break;
      }
    }
    umbrellaDetected = true;
    lastDetectionTime = millis();
    triggerNormalAlert("physical_detection");
  } else {
    if (millis() - lastDetectionTime > detectionTimeout) {
      triggerEmergency();
      lastDetectionTime = millis();
    }
  }

  if (millis() - lastWeatherSentTime >= weatherInterval || lastWeatherSentTime == 0) {
    fetchWeather();
    lastWeatherSentTime = millis();
  }

  if (rainExpectedWithoutUmbrella && !umbrellaDetected) {
    if (!secondRainWarningSent && millis() - rainWarningStartTime >= detectionTimeout) {
      mqttClient.publish(mqttTopicWeatherNotification, "🚨 Still raining & no umbrella after 2 mins!");
      Serial.println("Notification: Still raining & no umbrella after 2 mins!");
      triggerWeatherEmergency("No umbrella after rain");
      secondRainWarningSent = true;
    }
  }

  if (umbrellaDetected && rainExpectedWithoutUmbrella) {
    rainExpectedWithoutUmbrella = false;
    secondRainWarningSent = false;
    rainWarningStartTime = 0;
    Serial.println("Umbrella detected - resetting rain warning flags.");
  }

  delay(500);
}


void connectToMQTT() {
  while (!mqttClient.connected()) {
    Serial.println("Connecting to MQTT broker...");
    if (mqttClient.connect("ESP32WeatherClient")) {
      Serial.println("Connected to MQTT broker.");
    } else {
      Serial.print("Failed MQTT connection, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" retrying in 5 seconds...");
      delay(5000);
    }
  }
}

void blinkLED(int pin, int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(pin, HIGH);
    delay(delayMs);
    digitalWrite(pin, LOW);
    delay(delayMs);
  }
}


void handleButtonToggle() {
  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonState) {
    delay(debounceDelay);
    lastButtonState = reading;

    if (reading == LOW) {
      buttonDownTime = millis();
    } else {
      unsigned long pressDuration = millis() - buttonDownTime;

      if (pressDuration >= longPressDuration && !systemOn) {
        systemOn = true;
        mqttClient.publish(mqttTopicSystem, "✅ System ON");
        Serial.println("✅ System ON");
        blinkLED(GREEN_LED, 3, 150);
      } else if (pressDuration < longPressDuration && systemOn) {
        if (millis() - lastButtonClickTime < doubleClickGap) {
          systemOn = false;
          Serial.println("🔴 System OFF");
          blinkLED(RED_LED, 3, 150);
        }
        lastButtonClickTime = millis();
      }
    }
  }
}

void blinkRedLedForDuration(unsigned long durationMs, int blinkIntervalMs) {
  unsigned long startTime = millis();
  while (millis() - startTime < durationMs) {
    digitalWrite(RED_LED, HIGH);
    delay(blinkIntervalMs);
    digitalWrite(RED_LED, LOW);
    delay(blinkIntervalMs);
  }
}

void fetchWeather() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = String(weatherHost) + "?lat=" + latitude + "&lon=" + longitude + "&appid=" + apiKey;
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode == 200) {
      String payload = http.getString();

      StaticJsonDocument<1024> doc;
      DeserializationError error = deserializeJson(doc, payload);
      if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        http.end();
        return;
      }

      
      const char* description = doc["weather"][0]["description"];  
      float tempK = doc["main"]["temp"] | 0.0;                     
      float tempC = tempK - 273.15;                              
      int humidity = doc["main"]["humidity"] | 0;

      
      String message = "🌦️ Weather: ";
      message += String(description);
      message += ", Temp: ";
      message += String(tempC, 1); 
      message += "°C, Humidity: ";
      message += String(humidity);
      message += "%";


      mqttClient.publish(mqttTopicWeather, message.c_str());
      Serial.print("Weather update sent: ");
      Serial.println(message);

    
      String descStr = String(description);
      descStr.toLowerCase();
      if (descStr.indexOf("rain") >= 0) {
        rainExpectedWithoutUmbrella = true;
        rainWarningStartTime = millis();
        mqttClient.publish(mqttTopicWeatherNotification, "🌧️ Rain expected soon! Don't forget to bring your umbrella!");
        Serial.println("Notification: Rain expected soon!");
      } else {
        rainExpectedWithoutUmbrella = false;
        Serial.println("No rain expected.");
      }
    } else {
      Serial.print("HTTP error: ");
      Serial.println(httpCode);
    }
    http.end();
  }
}

int findRegisteredTagIndex(uint8_t* uid, uint8_t uidLength) {
  for (int i = 0; i < umbrellaCount; i++) {
    if (registeredUmbrellas[i].uidLength != uidLength) continue;
    bool match = true;
    for (int j = 0; j < uidLength; j++) {
      if (registeredUmbrellas[i].uid[j] != uid[j]) {
        match = false;
        break;
      }
    }
    if (match) return i;
  }
  return -1;
}


void registerNewUmbrella(uint8_t* uid, uint8_t uidLength) {
  if (umbrellaCount >= MAX_UMBRELLAS) {
    Serial.println("Max umbrellas reached! Triggering emergency alert.");

    digitalWrite(GREEN_LED, LOW);  

    unsigned long startTime = millis();
    while (millis() - startTime < 10000) {
      digitalWrite(RED_LED, HIGH);
      tone(BUZZER_PIN, 1000); 
      delay(500);
      digitalWrite(RED_LED, LOW);
      noTone(BUZZER_PIN);     
      delay(500);
    }

    mqttClient.publish(mqttTopicUmbrella, "❌ Umbrella rack full! Cannot register new umbrella.");
    mqttClient.publish(mqttTopicWeatherNotification, "❌ Umbrella rack full! Emergency alert triggered.");
    return;
  }
  
  for (int i = 0; i < uidLength; i++) registeredUmbrellas[umbrellaCount].uid[i] = uid[i];
  registeredUmbrellas[umbrellaCount].uidLength = uidLength;
  registeredUmbrellas[umbrellaCount].isOnStand = true;
  umbrellaCount++;

  String uidStr = "";
  for (uint8_t i = 0; i < uidLength; i++) uidStr += String(uid[i], HEX);

  mqttClient.publish(mqttTopicUmbrella, "🆕 New umbrella registered.");
  Serial.print("New umbrella registered, UID: ");
  Serial.println(uidStr);
}



void triggerNormalAlert(String message) {
  tone(BUZZER_PIN, 2000);      
  digitalWrite(GREEN_LED, HIGH);
  delay(1000);
  noTone(BUZZER_PIN);          
  digitalWrite(GREEN_LED, LOW);
}

void triggerDuplicateScanAlert() {
  Serial.println("Duplicate umbrella scan detected.");
  tone(BUZZER_PIN, 1500);      
  digitalWrite(GREEN_LED, HIGH);
  delay(9000);
  noTone(BUZZER_PIN);
  digitalWrite(GREEN_LED, LOW);
}

void triggerEmergency() {
  Serial.println("EMERGENCY: No umbrella detected for more than 2 minutes!");

  digitalWrite(GREEN_LED, LOW); 

  tone(BUZZER_PIN, 1000);     
  digitalWrite(RED_LED, HIGH);
  delay(10000);
  noTone(BUZZER_PIN);
  digitalWrite(RED_LED, LOW);
}

void triggerWeatherEmergency(String reason) {
  Serial.print("Weather Emergency triggered: ");
  Serial.println(reason);

  digitalWrite(GREEN_LED, LOW); 

  tone(BUZZER_PIN, 1000);      
  digitalWrite(RED_LED, HIGH);
  delay(10000);
  noTone(BUZZER_PIN);
  digitalWrite(RED_LED, LOW);
}


void handleObstacleSensor() {
  bool currentObstacleState = digitalRead(OBSTACLE_PIN);
  if (currentObstacleState != lastObstacleState) {
    lastObstacleState = currentObstacleState;
    Serial.print("Obstacle sensor state changed: ");
    Serial.println(currentObstacleState == LOW ? "DETECTED" : "NOT DETECTED");

    if (currentObstacleState == LOW) {
      mqttClient.publish(mqttTopicUmbrella, "☂️ Umbrella placed via sensor.");
      Serial.println("Umbrella placed detected via obstacle sensor, MQTT sent.");
      mqttClient.publish(mqttTopicWeatherNotification, "☂️ Umbrella placed (obstacle sensor).");
      Serial.println("Notification: Umbrella placed (obstacle sensor).");
      triggerNormalAlert("obstacle_detection");
    }
  }
}
