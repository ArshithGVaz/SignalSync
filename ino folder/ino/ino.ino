#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// =====================================================
// 1. WI-FI CREDENTIALS
// =====================================================
// Enter your Wi-Fi credentials locally.
// Do not share this file while it contains your password.
const char* WIFI_SSID = "Santocarlos";
const char* WIFI_PASSWORD = "123456788";

const char* FIREBASE_API_KEY = "AIzaSyDEIMjSPvZydbtwJetZUkKpoQNE-lGX2zg";
const char* FIREBASE_USER_EMAIL = "trafficlights@project.com";
const char* FIREBASE_USER_PASSWORD = "CONFIDENTIAL";


// =====================================================
// 2. FIREBASE DATABASE URLS
// =====================================================

const char* FIREBASE_URL =
  "https://traffic-light-86038-default-rtdb.asia-southeast1.firebasedatabase.app/trafficLight.json";

const char* CONTROL_URL =
  "https://traffic-light-86038-default-rtdb.asia-southeast1.firebasedatabase.app/control.json";

const char* STATUS_URL =
  "https://traffic-light-86038-default-rtdb.asia-southeast1.firebasedatabase.app/status.json";

const char* ADAPTIVE_TRAFFIC_URL =
  "https://traffic-light-86038-default-rtdb.asia-southeast1.firebasedatabase.app/adaptiveTraffic";

const char* FIREBASE_SIGNIN_URL =
  "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=";

const char* FIREBASE_REFRESH_URL =
  "https://securetoken.googleapis.com/v1/token?key=";

// =====================================================
// 3. TRAFFIC LIGHT CONFIGURATION
// =====================================================

struct LightConfig {
  int pin;
  unsigned long duration;
};

struct LaneConfig {
  LightConfig red;
  LightConfig yellow;
  LightConfig green;

  int irSensorPin;
};

LaneConfig lanes[3];
bool configLoaded = false;

// =====================================================
// 4. CONTROL CONFIGURATION
// =====================================================

String currentMode = "auto";
String currentCommand = "none";

unsigned long lastControlCheck = 0;
const unsigned long CONTROL_CHECK_INTERVAL = 1000;

uint64_t lastCommandId = 0;
bool controlInitialized = false;

// =====================================================
// 5. AUTOMATIC SEQUENCE CONFIGURATION
// =====================================================

enum Phase {
  LANE_1_GREEN,
  LANE_1_LANE_2_YELLOW,
  LANE_2_GREEN,
  LANE_2_LANE_3_YELLOW,
  LANE_3_GREEN,
  LANE_3_LANE_1_YELLOW
};

Phase currentPhase = LANE_1_GREEN;

unsigned long phaseStartTime = 0;
unsigned long phaseDuration = 0;

unsigned long lastFetchAttempt = 0;
const unsigned long FETCH_RETRY_INTERVAL = 5000;

// =====================================================
// 6. FIREBASE AUTHENTICATION CONFIGURATION
// =====================================================

String firebaseIdToken = "";
String firebaseRefreshToken = "";

unsigned long tokenExpiryTime = 0;

bool authenticationAttempted = false;
unsigned long lastAuthenticationAttempt = 0;
const unsigned long AUTH_RETRY_INTERVAL = 10000;

// =====================================================
// 7. STATUS CONFIGURATION
// =====================================================

unsigned long lastStatusPublish = 0;
const unsigned long STATUS_PUBLISH_INTERVAL = 1000;

bool statusPublishPending = true;

// =====================================================
// 7.1 CONGESTION DETECTION CONFIGURATION
// =====================================================

const unsigned long CONGESTION_DETECTION_TIME = 3000;

bool objectCurrentlyDetected[3] = {
  false, false, false
};

bool congestionDetected[3] = {
  false, false, false
};

unsigned long objectDetectionStart[3] = {
  0, 0, 0
};

// Time a lane must remain empty while RED before clearing congestion.
const unsigned long CONGESTION_CLEAR_TIME = 2000;

bool clearanceTiming[3] = {
  false, false, false
};

unsigned long clearanceStart[3] = {
  0, 0, 0
};

// =====================================================

// All adaptive durations are stored in SECONDS.

unsigned long adaptiveGreenDuration[3] = {0, 0, 0};

unsigned long originalGreenDuration[3] = {0, 0, 0};

unsigned long maxGreenDuration[3] = {60, 60, 60};

unsigned long incrementSeconds[3] = {5, 5, 5};

// Last adaptive decision for each lane.
String lastAdaptiveDecision[3] = {
  "normal",
  "normal",
  "normal"
};

// Whether we have successfully loaded adaptive settings.
bool adaptiveSettingsLoaded[3] = {false, false, false};

// =====================================================
// 7.2 FIREBASE CONGESTION SYNCHRONIZATION
// =====================================================

const unsigned long CONGESTION_SYNC_RETRY_INTERVAL = 5000;

bool congestionSyncPending[3] = {
  true, true, true
};

unsigned long lastCongestionSyncAttempt[3] = {
  0, 0, 0
};

// =====================================================
// 8. FUNCTION DECLARATIONS
// =====================================================

bool fetchConfiguration();
bool fetchControlCommand();

bool firebaseSignIn();
bool refreshFirebaseToken();
bool ensureFirebaseAuth();

bool publishStatus();

void setAllRed();
void allLightsOff();
void applyManualCommand();
void startPhase(Phase newPhase);
void updateTrafficLights();
void readIRSensors();
bool isLaneRed(int laneIndex);
bool syncCongestionState(int laneIndex);
bool updateAdaptiveGreenDuration(int laneIndex);

String getPhaseName();

bool isUsableOutputPin(int pin);
bool validatePins(LaneConfig candidate[]);

bool readLight(
  JsonVariantConst json,
  LightConfig &light,
  const char* lightName
);

bool readLane(
  JsonVariantConst json,
  LaneConfig &lane,
  int laneNumber
);

// =====================================================
// 9. GPIO VALIDATION
// =====================================================

bool isUsableOutputPin(int pin) {

  if (pin < 0 || pin > 33) {
    return false;
  }

  if (pin >= 6 && pin <= 11) {
    return false;
  }

  if (pin == 1 || pin == 3) {
    return false;
  }

  return true;
}

bool isUsableInputPin(int pin) {

  if (pin < 0 || pin > 39) {
    return false;
  }

  // GPIO 6–11 are reserved for ESP32 flash.
  return !(pin >= 6 && pin <= 11);
}

bool validatePins(LaneConfig candidate[]) {

  int outputPins[9] = {
    candidate[0].red.pin,
    candidate[0].yellow.pin,
    candidate[0].green.pin,

    candidate[1].red.pin,
    candidate[1].yellow.pin,
    candidate[1].green.pin,

    candidate[2].red.pin,
    candidate[2].yellow.pin,
    candidate[2].green.pin
  };

  int inputPins[3] = {
    candidate[0].irSensorPin,
    candidate[1].irSensorPin,
    candidate[2].irSensorPin
  };

  // Validate traffic-light output pins.
  for (int i = 0; i < 9; i++) {

    if (!isUsableOutputPin(outputPins[i])) {
      Serial.printf("Invalid output GPIO: %d\n", outputPins[i]);
      return false;
    }

    for (int j = i + 1; j < 9; j++) {

      if (outputPins[i] == outputPins[j]) {
        Serial.printf("Duplicate output GPIO: %d\n", outputPins[i]);
        return false;
      }
    }
  }

  // Validate IR sensor input pins.
  for (int i = 0; i < 3; i++) {

    if (!isUsableInputPin(inputPins[i])) {
      Serial.printf("Invalid IR sensor GPIO: %d\n", inputPins[i]);
      return false;
    }

    // Check conflicts with traffic-light pins.
    for (int j = 0; j < 9; j++) {

      if (inputPins[i] == outputPins[j]) {
        Serial.printf(
          "IR sensor GPIO %d conflicts with traffic-light GPIO!\n",
          inputPins[i]
        );

        return false;
      }
    }

    // Check duplicate IR sensor pins.
    for (int j = i + 1; j < 3; j++) {

      if (inputPins[i] == inputPins[j]) {
        Serial.printf(
          "Duplicate IR sensor GPIO: %d\n",
          inputPins[i]
        );

        return false;
      }
    }
  }

  return true;
}

// =====================================================
// 10. READ ONE LIGHT FROM FIREBASE
// =====================================================

bool readLight(
  JsonVariantConst json,
  LightConfig &light,
  const char* lightName
) {

  if (!json["pin"].is<int>() || !json["time"].is<int>()) {
    Serial.printf("Invalid JSON fields: %s\n", lightName);
    return false;
  }

  int pin = json["pin"].as<int>();
  int seconds = json["time"].as<int>();

  bool isRedLight = String(lightName).endsWith("_red");

  if (seconds < 0 || seconds > 3600) {
    Serial.printf("Invalid duration: %s\n", lightName);
    return false;
  }

  if (!isRedLight && seconds < 1) {
    Serial.printf("Invalid duration: %s\n", lightName);
    return false;
  }

  light.pin = pin;
  light.duration = (unsigned long)seconds * 1000UL;

  return true;
}

// =====================================================
// 11. READ ONE LANE
// =====================================================

// =====================================================
// READ ONE LANE
// =====================================================

bool readLane(
  JsonVariantConst json,
  LaneConfig &lane,
  int laneNumber
) {

  if (json.isNull()) {
    Serial.printf("Missing Lane_%d configuration.\n", laneNumber);
    return false;
  }

  char name[40];

  snprintf(name, sizeof(name), "Lane_%d_red", laneNumber);

  if (!readLight(json["red"], lane.red, name)) {
    return false;
  }

  snprintf(name, sizeof(name), "Lane_%d_yellow", laneNumber);

  if (!readLight(json["yellow"], lane.yellow, name)) {
    return false;
  }

  snprintf(name, sizeof(name), "Lane_%d_green", laneNumber);

  if (!readLight(json["green"], lane.green, name)) {
    return false;
  }

  // Read IR sensor GPIO.
  if (!json["irSensor"]["pin"].is<int>()) {

    Serial.printf(
      "Invalid IR sensor configuration for Lane_%d\n",
      laneNumber
    );

    return false;
  }

  lane.irSensorPin = json["irSensor"]["pin"].as<int>();

  return true;
}

// =====================================================
// 12. TURN OFF ALL CONFIGURED LEDS
// =====================================================

void allLightsOff() {

  if (!configLoaded) {
    return;
  }

  for (int i = 0; i < 3; i++) {

    digitalWrite(lanes[i].red.pin, LOW);
    digitalWrite(lanes[i].yellow.pin, LOW);
    digitalWrite(lanes[i].green.pin, LOW);
  }
}

// =====================================================
// 13. FETCH CONFIGURATION FROM FIREBASE
// =====================================================

bool syncAdaptiveMinimum(int laneIndex) {
  Serial.printf(
  "Attempting minimum sync for Lane %d...\n",
  laneIndex + 1
);
  if (laneIndex < 0 || laneIndex >= 3) {
    return false;
  }

  if (WiFi.status() != WL_CONNECTED || !ensureFirebaseAuth()) {
    Serial.println("Cannot sync adaptive minimum: Firebase unavailable.");
    return false;
  }

  String laneName = "Lane_" + String(laneIndex + 1);

  String url =
    String(ADAPTIVE_TRAFFIC_URL) +
    "/" + laneName +
    ".json?auth=" + firebaseIdToken;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  if (!http.begin(client, url)) {
    Serial.println("Adaptive minimum HTTP connection failed.");
    return false;
  }

  JsonDocument doc;
  doc["originalGreenDuration"] = originalGreenDuration[laneIndex];

  String payload;
  serializeJson(doc, payload);

  http.addHeader("Content-Type", "application/json");

  int httpCode = http.PATCH(payload);

  if (httpCode != HTTP_CODE_OK) {

    Serial.printf(
      "Lane %d minimum sync failed. HTTP: %d\n",
      laneIndex + 1,
      httpCode
    );

    String response = http.getString();

    if (response.length() > 0) {
      Serial.println(response);
    }

    http.end();

    if (httpCode == 401 || httpCode == 403) {
      firebaseIdToken = "";
      tokenExpiryTime = 0;
    }

    return false;
  }

  http.end();

  Serial.printf(
    "Lane %d minimum green duration synced: %lu seconds\n",
    laneIndex + 1,
    originalGreenDuration[laneIndex]
  );

  return true;
}


bool fetchConfiguration() {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Cannot fetch configuration.");
    return false;
  }

  // Ensure we have a valid Firebase ID token.
  if (!ensureFirebaseAuth()) {
    Serial.println("Firebase authentication failed. Cannot fetch configuration.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  // Attach Firebase authentication token.
  String authenticatedURL =
    String(FIREBASE_URL) + "?auth=" + firebaseIdToken;

  Serial.println("\nFetching Firebase configuration...");

  if (!http.begin(client, authenticatedURL)) {
    Serial.println("HTTP connection failed.");
    return false;
  }

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {

    Serial.printf("Configuration HTTP error: %d\n", httpCode);

    String response = http.getString();

    if (response.length() > 0) {
      Serial.println(response);
    }

    http.end();

    // Force authentication again on the next attempt.
    if (httpCode == 401 || httpCode == 403) {
      firebaseIdToken = "";
      tokenExpiryTime = 0;
    }

    return false;
  }

  String response = http.getString();
  http.end();

  JsonDocument doc;

  DeserializationError error = deserializeJson(doc, response);

  if (error) {

    Serial.print("JSON parsing failed: ");
    Serial.println(error.c_str());

    return false;
  }

  LaneConfig candidate[3];

  if (!readLane(doc["Lane_1"], candidate[0], 1)) return false;
  if (!readLane(doc["Lane_2"], candidate[1], 2)) return false;
  if (!readLane(doc["Lane_3"], candidate[2], 3)) return false;

  if (!validatePins(candidate)) {
    return false;
  }

  if (configLoaded) {
    setAllRed();
  }

  for (int i = 0; i < 3; i++) {
    lanes[i] = candidate[i];
  }

  
for (int i = 0; i < 3; i++) {

  originalGreenDuration[i] =
    lanes[i].green.duration / 1000UL;

  // Initialize local adaptive duration if this is
  // the first configuration load.
  if (adaptiveGreenDuration[i] == 0) {
    adaptiveGreenDuration[i] = originalGreenDuration[i];
  }

  // Synchronize the configurable minimum with Firebase.
  if (!syncAdaptiveMinimum(i)) {
    Serial.printf(
      "WARNING: Lane %d adaptive minimum was not synced.\n",
      i + 1
    );
  }
}


  for (int i = 0; i < 3; i++) {

    pinMode(lanes[i].red.pin, OUTPUT);
    pinMode(lanes[i].yellow.pin, OUTPUT);
    pinMode(lanes[i].green.pin, OUTPUT);

    // IR sensor input.
    pinMode(lanes[i].irSensorPin, INPUT); 
  }

  configLoaded = true;

  Serial.println("\nConfiguration loaded successfully!");

  for (int i = 0; i < 3; i++) {

    Serial.printf(
      "Lane %d | RED GPIO %d | YELLOW GPIO %d | GREEN GPIO %d\n",
      i + 1,
      lanes[i].red.pin,
      lanes[i].yellow.pin,
      lanes[i].green.pin
    );
    Serial.printf(
      "         IR SENSOR GPIO: %d\n",
      lanes[i].irSensorPin
    );

    Serial.printf(
      "         RED %lu sec | YELLOW %lu sec | GREEN %lu sec\n",
      lanes[i].red.duration / 1000UL,
      lanes[i].yellow.duration / 1000UL,
      lanes[i].green.duration / 1000UL
    );
  }

  return true;
}

// =====================================================
// READ IR SENSOR STATES
// =====================================================


bool isLaneRed(int laneIndex) {

  if (!configLoaded || laneIndex < 0 || laneIndex > 2) {
    return false;
  }

  // MANUAL MODE
  if (currentMode == "manual") {

    if (currentCommand == "all_red" ||
        currentCommand == "none") {
      return true;
    }

    if (currentCommand == "lane_1") {
      return laneIndex != 0;
    }

    if (currentCommand == "lane_2") {
      return laneIndex != 1;
    }

    if (currentCommand == "lane_3") {
      return laneIndex != 2;
    }

    return false;
  }

  // AUTOMATIC MODE
  if (currentMode != "auto") {
    return false;
  }

  switch (currentPhase) {

    case LANE_1_GREEN:
      return laneIndex != 0;

    case LANE_1_LANE_2_YELLOW:
      return laneIndex == 2;

    case LANE_2_GREEN:
      return laneIndex != 1;

    case LANE_2_LANE_3_YELLOW:
      return laneIndex == 0;

    case LANE_3_GREEN:
      return laneIndex != 2;

    case LANE_3_LANE_1_YELLOW:
      return laneIndex == 1;
  }

  return false;
}


void readIRSensors() {

  if (!configLoaded) {
    return;
  }

  for (int i = 0; i < 3; i++) {

    int sensorState = digitalRead(lanes[i].irSensorPin);

    // Your IR sensors are active LOW.
    bool objectDetected = (sensorState == LOW);

    // =============================================
    // 1. OBJECT DETECTED
    // =============================================

    if (objectDetected && !objectCurrentlyDetected[i]) {

      objectCurrentlyDetected[i] = true;

      objectDetectionStart[i] = millis();

      Serial.printf(
        "Lane %d: OBJECT DETECTED\n",
        i + 1
      );
    }

    // =============================================
    // 2. OBJECT REMOVED
    // =============================================

    else if (!objectDetected && objectCurrentlyDetected[i]) {

      objectCurrentlyDetected[i] = false;

      Serial.printf(
        "Lane %d: OBJECT REMOVED\n",
        i + 1
      );
    }

    // =============================================
    // 3. DETECT CONGESTION AFTER 3 SECONDS
    // =============================================

    if (
      objectCurrentlyDetected[i] &&
      !congestionDetected[i] &&
      millis() - objectDetectionStart[i] >=
        CONGESTION_DETECTION_TIME
    ) {

      congestionDetected[i] = true;

      // Schedule Firebase synchronization.
      congestionSyncPending[i] = true;

      Serial.printf(
        "Lane %d: CONGESTION DETECTED!\n",
        i + 1
      );
    }

    // =============================================
    // 4. CHECK CONGESTION CLEARANCE
    // =============================================

    bool laneIsCurrentlyRed = isLaneRed(i);

    if (
      congestionDetected[i] &&
      laneIsCurrentlyRed &&
      !objectDetected
    ) {

      // Start clearance timer.
      if (!clearanceTiming[i]) {

        clearanceTiming[i] = true;

        clearanceStart[i] = millis();

        Serial.printf(
          "Lane %d: RED + SENSOR CLEAR. Checking clearance...\n",
          i + 1
        );
      }

      // Check whether the lane remained clear for 2 seconds.
      else if (
        millis() - clearanceStart[i] >=
          CONGESTION_CLEAR_TIME
      ) {

        congestionDetected[i] = false;

        // Schedule Firebase synchronization.
        congestionSyncPending[i] = true;


        clearanceTiming[i] = false;
        clearanceStart[i] = 0;

        Serial.printf(
          "Lane %d: CONGESTION CLEARED!\n",
          i + 1
        );
      }
    }

    // =============================================
    // 5. CANCEL CLEARANCE TIMER IF CONDITIONS CHANGE
    // =============================================

    else {

      if (clearanceTiming[i]) {

        clearanceTiming[i] = false;
        clearanceStart[i] = 0;

        Serial.printf(
          "Lane %d: Clearance check cancelled.\n",
          i + 1
        );
      }
    }
    // =============================================
    // 6. SYNCHRONIZE CONGESTION STATE WITH FIREBASE
    // =============================================

    if (
      congestionSyncPending[i] &&
      millis() - lastCongestionSyncAttempt[i] >=
        CONGESTION_SYNC_RETRY_INTERVAL
    ) {

      lastCongestionSyncAttempt[i] = millis();

      if (syncCongestionState(i)) {

        congestionSyncPending[i] = false;
      }
    }
  }
}

// =====================================================
// 14. SET ALL LANES TO RED
// =====================================================

void setAllRed() {

  if (!configLoaded) {
    return;
  }

  for (int i = 0; i < 3; i++) {

    digitalWrite(lanes[i].red.pin, HIGH);
    digitalWrite(lanes[i].yellow.pin, LOW);
    digitalWrite(lanes[i].green.pin, LOW);
  }

  statusPublishPending = true;
}

// =====================================================
// 15. MANUAL TRAFFIC LIGHT CONTROL
// =====================================================

void applyManualCommand() {

  if (!configLoaded) {
    Serial.println("Configuration not loaded.");
    return;
  }

  setAllRed();

  if (currentCommand == "all_red") {

    Serial.println("MANUAL: All lanes RED");
  }

  else if (currentCommand == "all_yellow") {

    for (int i = 0; i < 3; i++) {

      digitalWrite(lanes[i].red.pin, LOW);
      digitalWrite(lanes[i].yellow.pin, HIGH);
      digitalWrite(lanes[i].green.pin, LOW);
    }

    Serial.println("MANUAL: All lanes YELLOW");
  }

  else if (currentCommand == "lane_1") {

    digitalWrite(lanes[0].red.pin, LOW);
    digitalWrite(lanes[0].green.pin, HIGH);

    Serial.println("MANUAL: Lane 1 GREEN");
  }

  else if (currentCommand == "lane_2") {

    digitalWrite(lanes[1].red.pin, LOW);
    digitalWrite(lanes[1].green.pin, HIGH);

    Serial.println("MANUAL: Lane 2 GREEN");
  }

  else if (currentCommand == "lane_3") {

    digitalWrite(lanes[2].red.pin, LOW);
    digitalWrite(lanes[2].green.pin, HIGH);

    Serial.println("MANUAL: Lane 3 GREEN");
  }

  else if (currentCommand == "none") {

    Serial.println("MANUAL: No command selected; all lanes RED");
  }

  else {

    Serial.print("Unknown manual command: ");
    Serial.println(currentCommand);

    setAllRed();
  }

  statusPublishPending = true;
}

// =====================================================
// 16. START AUTOMATIC PHASE
// =====================================================

void startPhase(Phase newPhase) {

  if (!configLoaded) {
    Serial.println("Cannot start phase: configuration not loaded.");
    return;
  }

  currentPhase = newPhase;

  setAllRed();

  switch (currentPhase) {

    case LANE_1_GREEN:

      if (!updateAdaptiveGreenDuration(0)) {
        Serial.println("Adaptive timer unavailable for Lane 1. Using previous duration.");
      }

      digitalWrite(lanes[0].red.pin, LOW);
      digitalWrite(lanes[0].green.pin, HIGH);

      phaseDuration = adaptiveGreenDuration[0] * 1000UL;

      Serial.println("\nLANE 1 GREEN | LANE 2 RED | LANE 3 RED");
      break;

    case LANE_1_LANE_2_YELLOW:

      digitalWrite(lanes[0].red.pin, LOW);
      digitalWrite(lanes[0].yellow.pin, HIGH);

      digitalWrite(lanes[1].red.pin, LOW);
      digitalWrite(lanes[1].yellow.pin, HIGH);

      phaseDuration = max(
        lanes[0].yellow.duration,
        lanes[1].yellow.duration
      );

      Serial.println("\nLANE 1 YELLOW | LANE 2 YELLOW | LANE 3 RED");
      break;

    case LANE_2_GREEN:

      if (!updateAdaptiveGreenDuration(1)) {
        Serial.println("Adaptive timer unavailable for Lane 2. Using previous duration.");
      }

      digitalWrite(lanes[1].red.pin, LOW);
      digitalWrite(lanes[1].green.pin, HIGH);

      phaseDuration = adaptiveGreenDuration[1] * 1000UL;

      Serial.println("\nLANE 1 RED | LANE 2 GREEN | LANE 3 RED");
      break;

    case LANE_2_LANE_3_YELLOW:

      digitalWrite(lanes[1].red.pin, LOW);
      digitalWrite(lanes[1].yellow.pin, HIGH);

      digitalWrite(lanes[2].red.pin, LOW);
      digitalWrite(lanes[2].yellow.pin, HIGH);

      phaseDuration = max(
        lanes[1].yellow.duration,
        lanes[2].yellow.duration
      );

      Serial.println("\nLANE 1 RED | LANE 2 YELLOW | LANE 3 YELLOW");
      break;

    case LANE_3_GREEN:

      if (!updateAdaptiveGreenDuration(2)) {
        Serial.println("Adaptive timer unavailable for Lane 3. Using previous duration.");
      }

      digitalWrite(lanes[2].red.pin, LOW);
      digitalWrite(lanes[2].green.pin, HIGH);

      phaseDuration = adaptiveGreenDuration[2] * 1000UL;

      Serial.println("\nLANE 1 RED | LANE 2 RED | LANE 3 GREEN");
      break;

    case LANE_3_LANE_1_YELLOW:

      digitalWrite(lanes[2].red.pin, LOW);
      digitalWrite(lanes[2].yellow.pin, HIGH);

      digitalWrite(lanes[0].red.pin, LOW);
      digitalWrite(lanes[0].yellow.pin, HIGH);

      phaseDuration = max(
        lanes[2].yellow.duration,
        lanes[0].yellow.duration
      );

      Serial.println("\nLANE 1 YELLOW | LANE 2 RED | LANE 3 YELLOW");
      break;
  }

  phaseStartTime = millis();

  statusPublishPending = true;

  Serial.printf(
    "Phase duration: %lu seconds\n",
    phaseDuration / 1000UL
  );
}

// =====================================================
// 17. UPDATE AUTOMATIC SEQUENCE
// =====================================================

void updateTrafficLights() {

  if (!configLoaded || currentMode != "auto") {
    return;
  }

  if (millis() - phaseStartTime < phaseDuration) {
    return;
  }

  switch (currentPhase) {

    case LANE_1_GREEN:
      startPhase(LANE_1_LANE_2_YELLOW);
      break;

    case LANE_1_LANE_2_YELLOW:
      startPhase(LANE_2_GREEN);
      break;

    case LANE_2_GREEN:
      startPhase(LANE_2_LANE_3_YELLOW);
      break;

    case LANE_2_LANE_3_YELLOW:
      startPhase(LANE_3_GREEN);
      break;

    case LANE_3_GREEN:
      startPhase(LANE_3_LANE_1_YELLOW);
      break;

    case LANE_3_LANE_1_YELLOW:

      Serial.println("\n========== NEW CYCLE ==========");

      if (!fetchConfiguration()) {

        Serial.println("Configuration fetch failed.");

        allLightsOff();
        configLoaded = false;
        lastFetchAttempt = millis();

        statusPublishPending = true;

        return;
      }

      startPhase(LANE_1_GREEN);
      break;
  }
}

// =====================================================
// 18. FETCH CONTROL COMMAND
// =====================================================


bool fetchControlCommand() {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Cannot fetch control.");
    return false;
  }

  // Ensure we have a valid Firebase ID token.
  if (!ensureFirebaseAuth()) {
    Serial.println("Firebase authentication failed. Cannot fetch control.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  // Attach Firebase authentication token.
  String authenticatedURL =
    String(CONTROL_URL) + "?auth=" + firebaseIdToken;

  if (!http.begin(client, authenticatedURL)) {
    Serial.println("Control connection failed.");
    return false;
  }

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {

    Serial.printf("Control fetch failed: %d\n", httpCode);

    String response = http.getString();

    if (response.length() > 0) {
      Serial.println(response);
    }

    http.end();

    // Force authentication again on the next attempt.
    if (httpCode == 401 || httpCode == 403) {
      firebaseIdToken = "";
      tokenExpiryTime = 0;
    }

    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;

  DeserializationError error = deserializeJson(doc, payload);

  if (error) {

    Serial.print("Control JSON parsing failed: ");
    Serial.println(error.c_str());

    return false;
  }

  String receivedMode = doc["mode"] | "auto";
  String receivedCommand = doc["manualCommand"] | "none";

  uint64_t receivedCommandId = doc["commandId"] | 0ULL;

  if (!controlInitialized || receivedCommandId != lastCommandId) {

    controlInitialized = true;
    lastCommandId = receivedCommandId;

    currentMode = receivedMode;
    currentCommand = receivedCommand;

    Serial.println("\n========== NEW CONTROL STATE ==========");

    Serial.print("Mode: ");
    Serial.println(currentMode);

    Serial.print("Command: ");
    Serial.println(currentCommand);

    if (!configLoaded) {

      Serial.println("Configuration not loaded yet.");
    }

    else if (currentMode == "manual") {

      applyManualCommand();
    }

    else if (currentMode == "auto") {

      Serial.println("Switching to AUTOMATIC mode.");

      startPhase(LANE_1_GREEN);
    }

    else {

      Serial.println("Unknown mode. Keeping all lanes RED.");

      setAllRed();
    }

    statusPublishPending = true;

    Serial.println("========================================");
  }

  return true;
}


// =====================================================
// 19. FIREBASE SIGN-IN
// =====================================================

bool firebaseSignIn() {

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String url = String(FIREBASE_SIGNIN_URL) + FIREBASE_API_KEY;

  if (!http.begin(client, url)) {
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  JsonDocument request;

  request["email"] = FIREBASE_USER_EMAIL;
  request["password"] = FIREBASE_USER_PASSWORD;
  request["returnSecureToken"] = true;

  String body;
  serializeJson(request, body);

  Serial.println("Signing in to Firebase...");

  int httpCode = http.POST(body);

  String response = http.getString();
  http.end();

  if (httpCode != HTTP_CODE_OK) {

    Serial.printf("Firebase sign-in failed: %d\n", httpCode);
    Serial.println(response);

    return false;
  }

  JsonDocument doc;

  if (deserializeJson(doc, response)) {
    Serial.println("Authentication response parsing failed.");
    return false;
  }

  firebaseIdToken = doc["idToken"] | "";
  firebaseRefreshToken = doc["refreshToken"] | "";

  unsigned long expiresIn =
    strtoul(doc["expiresIn"] | "3600", nullptr, 10);

  if (firebaseIdToken.length() == 0 ||
      firebaseRefreshToken.length() == 0) {

    Serial.println("Authentication tokens missing.");
    return false;
  }

  tokenExpiryTime = millis() +
    ((expiresIn > 60 ? expiresIn - 60 : 1) * 1000UL);

  Serial.println("Firebase authentication successful!");

  authenticationAttempted = true;

  return true;
}

// =====================================================
// 20. REFRESH FIREBASE TOKEN
// =====================================================

bool refreshFirebaseToken() {

  if (WiFi.status() != WL_CONNECTED ||
      firebaseRefreshToken.length() == 0) {

    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String url = String(FIREBASE_REFRESH_URL) + FIREBASE_API_KEY;

  if (!http.begin(client, url)) {
    return false;
  }

  http.addHeader(
    "Content-Type",
    "application/x-www-form-urlencoded"
  );

  String body =
    "grant_type=refresh_token&refresh_token=" +
    firebaseRefreshToken;

  int httpCode = http.POST(body);

  String response = http.getString();
  http.end();

  if (httpCode != HTTP_CODE_OK) {

    Serial.printf("Token refresh failed: %d\n", httpCode);

    return false;
  }

  JsonDocument doc;

  if (deserializeJson(doc, response)) {
    Serial.println("Token refresh parsing failed.");
    return false;
  }

  firebaseIdToken = doc["id_token"] | "";

  String newRefreshToken = doc["refresh_token"] | "";

  if (newRefreshToken.length() > 0) {
    firebaseRefreshToken = newRefreshToken;
  }

  unsigned long expiresIn =
    strtoul(doc["expires_in"] | "3600", nullptr, 10);

  if (firebaseIdToken.length() == 0) {
    return false;
  }

  tokenExpiryTime = millis() +
    ((expiresIn > 60 ? expiresIn - 60 : 1) * 1000UL);

  Serial.println("Firebase token refreshed.");

  return true;
}

// =====================================================
// 21. ENSURE VALID FIREBASE TOKEN
// =====================================================

bool ensureFirebaseAuth() {

  if (firebaseIdToken.length() > 0 &&
      (long)(tokenExpiryTime - millis()) > 0) {

    return true;
  }

  if (firebaseRefreshToken.length() > 0) {

    if (refreshFirebaseToken()) {
      return true;
    }
  }

  return firebaseSignIn();
}


bool updateAdaptiveGreenDuration(int laneIndex) {

  if (laneIndex < 0 || laneIndex >= 3) {
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot update adaptive timer: WiFi disconnected.");
    return false;
  }

  if (!ensureFirebaseAuth()) {
    Serial.println("Cannot update adaptive timer: Firebase authentication failed.");
    return false;
  }

  String laneName = "Lane_" + String(laneIndex + 1);

  String laneURL =
    String(ADAPTIVE_TRAFFIC_URL) +
    "/" + laneName +
    ".json?auth=" + firebaseIdToken;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  // =========================================
  // 1. FETCH LATEST ADAPTIVE LANE DATA
  // =========================================

  if (!http.begin(client, laneURL)) {
    Serial.println("Adaptive timer GET connection failed.");
    return false;
  }

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {

    Serial.printf(
      "Adaptive timer GET failed. HTTP: %d\n",
      httpCode
    );

    Serial.println(http.getString());

    http.end();

    if (httpCode == 401 || httpCode == 403) {
      firebaseIdToken = "";
      tokenExpiryTime = 0;
    }

    return false;
  }

  String response = http.getString();
  http.end();

  JsonDocument doc;

  DeserializationError error = deserializeJson(doc, response);

  if (error || !doc.is<JsonObject>()) {
    Serial.println("Invalid adaptive traffic JSON.");
    return false;
  }

  // =========================================
  // 2. READ CURRENT SETTINGS
  // =========================================

  bool congested = doc["congested"] | false;

  unsigned long currentDuration =
    doc["greenDuration"] | originalGreenDuration[laneIndex];

  unsigned long maximum =
    doc["maxGreenDuration"] | maxGreenDuration[laneIndex];

  unsigned long increment =
    doc["incrementSeconds"] | incrementSeconds[laneIndex];

  unsigned long original =
    originalGreenDuration[laneIndex];

  if (increment == 0 || maximum < original) {
    Serial.println("Invalid adaptive timer settings.");
    return false;
  }

  // Keep the current duration within valid limits.
  currentDuration = constrain(
    currentDuration,
    original,
    maximum
  );

  // =========================================
  // 3. CALCULATE NEW GREEN DURATION
  // =========================================

  unsigned long newDuration = currentDuration;

  String decision = "normal";

  if (congested) {

    if (currentDuration < maximum) {

      unsigned long remainingCapacity =
        maximum - currentDuration;

      if (increment >= remainingCapacity) {
        newDuration = maximum;
      } else {
        newDuration = currentDuration + increment;
      }

      decision = "increased";

    } else {
      decision = "max_reached";
    }

  } else {

    if (currentDuration > original) {

      unsigned long reduction =
        currentDuration - original;

      if (increment >= reduction) {
        newDuration = original;
      } else {
        newDuration = currentDuration - increment;
      }

      decision = "decreased";

    } else {
      decision = "normal";
    }
  }

  // =========================================
  // 4. SAVE UPDATED TIMER TO FIREBASE
  // =========================================

  JsonDocument updateDoc;

  updateDoc["greenDuration"] = newDuration;
  updateDoc["lastDecision"] = decision;

  String payload;
  serializeJson(updateDoc, payload);

  if (!http.begin(client, laneURL)) {
    Serial.println("Adaptive timer PATCH connection failed.");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  int patchCode = http.PATCH(payload);

  if (patchCode != HTTP_CODE_OK) {

    Serial.printf(
      "Adaptive timer PATCH failed. HTTP: %d\n",
      patchCode
    );

    Serial.println(http.getString());

    http.end();

    if (patchCode == 401 || patchCode == 403) {
      firebaseIdToken = "";
      tokenExpiryTime = 0;
    }

    return false;
  }

  http.end();

  // =========================================
  // 5. UPDATE LOCAL ADAPTIVE VARIABLES
  // =========================================

  adaptiveGreenDuration[laneIndex] = newDuration;

  maxGreenDuration[laneIndex] = maximum;

  incrementSeconds[laneIndex] = increment;

  lastAdaptiveDecision[laneIndex] = decision;

  adaptiveSettingsLoaded[laneIndex] = true;

  Serial.println("\n========== ADAPTIVE TIMER ==========");

  Serial.printf("Lane: %s\n", laneName.c_str());

  Serial.printf("Congested: %s\n", congested ? "YES" : "NO");

  Serial.printf("Original duration: %lu seconds\n", original);

  Serial.printf("Previous duration: %lu seconds\n", currentDuration);

  Serial.printf("New duration: %lu seconds\n", newDuration);

  Serial.printf("Maximum duration: %lu seconds\n", maximum);

  Serial.printf("Increment: %lu seconds\n", increment);

  Serial.printf("Decision: %s\n", decision.c_str());

  Serial.println("Firebase adaptive timer updated.");
  Serial.println("====================================\n");

  return true;
}



// =====================================================
// 21.1 SYNC CONGESTION STATE TO FIREBASE
// =====================================================

bool syncCongestionState(int laneIndex) {

  if (laneIndex < 0 || laneIndex > 2) {
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (!ensureFirebaseAuth()) {
    Serial.println("Cannot sync congestion: Firebase authentication failed.");
    return false;
  }

  // Construct the lane-specific Firebase URL.
  String laneName = "Lane_" + String(laneIndex + 1);

  String url =
    String(ADAPTIVE_TRAFFIC_URL) +
    "/" + laneName +
    ".json?auth=" + firebaseIdToken;

  // Only update the congestion field.
  JsonDocument doc;

  doc["congested"] = congestionDetected[laneIndex];

  String payload;
  serializeJson(doc, payload);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  if (!http.begin(client, url)) {
    Serial.println("Could not connect to adaptive traffic URL.");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  int httpCode = http.PATCH(payload);

  String response = "";

  if (httpCode != HTTP_CODE_OK) {
    response = http.getString();
  }

  http.end();

  if (httpCode == HTTP_CODE_OK) {

    Serial.printf(
      "Firebase updated: %s congestion = %s\n",
      laneName.c_str(),
      congestionDetected[laneIndex] ? "true" : "false"
    );

    return true;
  }

  Serial.printf(
    "Congestion sync failed for %s. HTTP: %d\n",
    laneName.c_str(),
    httpCode
  );

  if (response.length() > 0) {
    Serial.println(response);
  }

  if (httpCode == 401 || httpCode == 403) {
    firebaseIdToken = "";
    tokenExpiryTime = 0;
  }

  return false;
}

// =====================================================
// 22. GET CURRENT PHASE NAME
// =====================================================

String getPhaseName() {

  if (currentMode == "manual") {

    if (currentCommand == "all_red") return "manual_all_red";
    if (currentCommand == "all_yellow") return "manual_all_yellow";
    if (currentCommand == "lane_1") return "manual_lane_1";
    if (currentCommand == "lane_2") return "manual_lane_2";
    if (currentCommand == "lane_3") return "manual_lane_3";

    return "manual_none";
  }

  switch (currentPhase) {

    case LANE_1_GREEN:
      return "lane_1_green";

    case LANE_1_LANE_2_YELLOW:
      return "lane_1_lane_2_yellow";

    case LANE_2_GREEN:
      return "lane_2_green";

    case LANE_2_LANE_3_YELLOW:
      return "lane_2_lane_3_yellow";

    case LANE_3_GREEN:
      return "lane_3_green";

    case LANE_3_LANE_1_YELLOW:
      return "lane_3_lane_1_yellow";
  }

  return "unknown";
}

// =====================================================
// 23. PUBLISH LIVE STATUS TO FIREBASE
// =====================================================

bool publishStatus() {

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (!ensureFirebaseAuth()) {

    Serial.println("Cannot publish: Firebase authentication failed.");

    return false;
  }

  String lane1 = "red";
  String lane2 = "red";
  String lane3 = "red";

  int currentLane = 0;

  JsonDocument doc;

  JsonArray activeLanes = doc["activeLanes"].to<JsonArray>();

  // AUTOMATIC MODE

  if (configLoaded && currentMode == "auto") {

    switch (currentPhase) {

      case LANE_1_GREEN:

        lane1 = "green";
        currentLane = 1;
        activeLanes.add(1);
        break;

      case LANE_1_LANE_2_YELLOW:

        lane1 = "yellow";
        lane2 = "yellow";

        activeLanes.add(1);
        activeLanes.add(2);
        break;

      case LANE_2_GREEN:

        lane2 = "green";
        currentLane = 2;
        activeLanes.add(2);
        break;

      case LANE_2_LANE_3_YELLOW:

        lane2 = "yellow";
        lane3 = "yellow";

        activeLanes.add(2);
        activeLanes.add(3);
        break;

      case LANE_3_GREEN:

        lane3 = "green";
        currentLane = 3;
        activeLanes.add(3);
        break;

      case LANE_3_LANE_1_YELLOW:

        lane3 = "yellow";
        lane1 = "yellow";

        activeLanes.add(3);
        activeLanes.add(1);
        break;
    }
  }

  // MANUAL MODE

  else if (configLoaded && currentMode == "manual") {

    if (currentCommand == "all_yellow") {

      lane1 = "yellow";
      lane2 = "yellow";
      lane3 = "yellow";

      activeLanes.add(1);
      activeLanes.add(2);
      activeLanes.add(3);
    }

    else if (currentCommand == "lane_1") {

      lane1 = "green";
      currentLane = 1;
      activeLanes.add(1);
    }

    else if (currentCommand == "lane_2") {

      lane2 = "green";
      currentLane = 2;
      activeLanes.add(2);
    }

    else if (currentCommand == "lane_3") {

      lane3 = "green";
      currentLane = 3;
      activeLanes.add(3);
    }
  }

  // REMAINING TIME

  unsigned long remainingSeconds = 0;

  if (configLoaded && currentMode == "auto") {

    unsigned long elapsed = millis() - phaseStartTime;

    if (elapsed < phaseDuration) {

      unsigned long remainingMilliseconds =
        phaseDuration - elapsed;

      remainingSeconds =
        (remainingMilliseconds + 999UL) / 1000UL;
    }
  }

  // BUILD STATUS JSON

  doc["deviceOnline"] = true;
  doc["configLoaded"] = configLoaded;
  doc["mode"] = currentMode;
  doc["phase"] = configLoaded ? getPhaseName() : "config_not_loaded";
  doc["currentLane"] = currentLane;
  doc["remainingSeconds"] = remainingSeconds;
  // Firebase resolves this server-value placeholder to its own Unix
  // timestamp in milliseconds. This avoids relying on the ESP32 clock.
  JsonObject serverTimestamp = doc["lastUpdated"].to<JsonObject>();
  serverTimestamp[".sv"] = "timestamp";

  JsonObject laneStates =
    doc["laneStates"].to<JsonObject>();

  laneStates["Lane_1"] = lane1;
  laneStates["Lane_2"] = lane2;
  laneStates["Lane_3"] = lane3;

  String payload;
  serializeJson(doc, payload);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String authenticatedStatusURL =
  String(STATUS_URL) + "?auth=" + firebaseIdToken;

if (!http.begin(client, authenticatedStatusURL)) {
  Serial.println("Could not connect to Firebase status URL.");
  return false;
}

http.addHeader("Content-Type", "application/json");

int httpCode = http.PUT(payload);

  String response = "";

  if (httpCode != HTTP_CODE_OK) {
    response = http.getString();
  }

  http.end();

  if (httpCode == HTTP_CODE_OK) {

    Serial.println("Traffic status published successfully.");

    return true;
  }

  Serial.printf("Status publishing failed. HTTP: %d\n", httpCode);

  if (response.length() > 0) {
    Serial.println(response);
  }

  if (httpCode == 401 || httpCode == 403) {

    firebaseIdToken = "";
    tokenExpiryTime = 0;
  }

  return false;
}

// =====================================================
// 24. SETUP
// =====================================================

void setup() {

  Serial.begin(115200);
  delay(300);

  Serial.println("\nTHREE-LANE TRAFFIC LIGHT SYSTEM");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED) {

    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected!");

  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Firebase sign-in will occur when status is first published.

  if (fetchConfiguration()) {

    startPhase(LANE_1_GREEN);
  }

  else {

    Serial.println("Initial configuration failed.");
    Serial.println("Will retry in 5 seconds.");

    configLoaded = false;
    lastFetchAttempt = millis();
  }

  lastControlCheck = millis() - CONTROL_CHECK_INTERVAL;
  lastStatusPublish = millis() - STATUS_PUBLISH_INTERVAL;
}

// =====================================================
// 25. MAIN LOOP
// =====================================================

void loop() {

  // 1. Reconnect Wi-Fi if disconnected.

  if (WiFi.status() != WL_CONNECTED) {

    static unsigned long lastWiFiRetry = 0;

    if (millis() - lastWiFiRetry >= 5000) {

      lastWiFiRetry = millis();

      Serial.println("WiFi disconnected. Reconnecting...");

      WiFi.reconnect();
    }

    return;
  }

  // 2. Poll Firebase control.

  if (millis() - lastControlCheck >= CONTROL_CHECK_INTERVAL) {

    lastControlCheck = millis();

    fetchControlCommand();
  }

  // 3. Retry configuration if needed.

  if (!configLoaded) {

    if (millis() - lastFetchAttempt >= FETCH_RETRY_INTERVAL) {

      lastFetchAttempt = millis();

      if (fetchConfiguration()) {

        Serial.println("Configuration restored successfully!");

        if (currentMode == "manual") {

          applyManualCommand();
        }

        else {

          startPhase(LANE_1_GREEN);
        }
      }
    }
  }

  // 4. Update automatic sequence.

  if (configLoaded && currentMode == "auto") {

    updateTrafficLights();
  }

  // 5. Publish live status.

  if (millis() - lastStatusPublish >= STATUS_PUBLISH_INTERVAL) {

    lastStatusPublish = millis();

    publishStatus();
  }
  readIRSensors();
}