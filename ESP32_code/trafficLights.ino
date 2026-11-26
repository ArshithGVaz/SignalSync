#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// =====================================================
// 1. WI-FI CREDENTIALS
// =====================================================
// Enter your Wi-Fi credentials locally.
// Do not share this file while it contains your password.
const char* WIFI_SSID = "Password_is_1_to_8_thrice";
const char* WIFI_PASSWORD = "128128128";

const char* FIREBASE_API_KEY = "****************************";
const char* FIREBASE_USER_EMAIL = "*************************";
const char* FIREBASE_USER_PASSWORD = "**********************";


// =====================================================
// 2. FIREBASE DATABASE URLS
// =====================================================

const char* FIREBASE_URL =
  "https://traffic-light-86038-default-rtdb.asia-southeast1.firebasedatabase.app/trafficLight.json";

const char* CONTROL_URL =
  "https://traffic-light-86038-default-rtdb.asia-southeast1.firebasedatabase.app/control.json";

const char* STATUS_URL =
  "https://traffic-light-86038-default-rtdb.asia-southeast1.firebasedatabase.app/status.json";

const char* FIREBASE_SIGNIN_URL =
  "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=";

const char* FIREBASE_REFRESH_URL =
  "https://securetoken.googleapis.com/v1/token?key=";

const char* EMERGENCY_URL =
  "https://traffic-light-86038-default-rtdb.asia-southeast1.firebasedatabase.app/emergency.json";

const char* ADAPTIVE_BASE_URL =
  "https://traffic-light-86038-default-rtdb.asia-southeast1.firebasedatabase.app/adaptiveTraffic";

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
};

LaneConfig lanes[3];
bool configLoaded = false;

// =====================================================
// IR SENSOR + ADAPTIVE TRAFFIC CONFIGURATION
// =====================================================

// IR sensor output pins for Lane 1, Lane 2 and Lane 3.
// This code assumes the sensors are ACTIVE LOW.
const int IR_PINS[3] = {27, 26, 32};

bool vehicleDetected[3] = {false, false, false};
bool laneCongested[3] = {false, false, false};

unsigned long lastSensorPrint = 0;
const unsigned long SENSOR_PRINT_INTERVAL = 1000;

unsigned long adaptiveGreenDurationSec[3] = {0, 0, 0};
unsigned long originalGreenDurationSec[3] = {0, 0, 0};
unsigned long maxGreenDurationSec[3] = {60, 60, 60};
unsigned long incrementSeconds[3] = {5, 5, 5};

String adaptiveLastDecision[3] = {
  "normal", "normal", "normal"
};

bool adaptiveConfigLoaded = false;
unsigned long lastAdaptiveConfigAttempt = 0;
const unsigned long ADAPTIVE_CONFIG_RETRY_INTERVAL = 5000;

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
// EMERGENCY CONFIGURATION
// =====================================================

// How frequently the Traffic-Light ESP32 checks Firebase
const unsigned long EMERGENCY_CHECK_INTERVAL = 1000;

// Emergency heartbeat expires after 5 seconds without an update
const unsigned long EMERGENCY_HEARTBEAT_TIMEOUT = 5000;

unsigned long lastEmergencyCheck = 0;

bool emergencyDetected = false;
bool emergencyHeartbeatValid = false;

int emergencyPriorityLane = 0;

uint64_t emergencyLastHeartbeat = 0;
uint64_t emergencyHeartbeatAge = 0;

bool validEmergencyActive = false;


// =====================================================
// EMERGENCY STATE MACHINE
// =====================================================

enum EmergencyState {
  EMERGENCY_IDLE,
  EMERGENCY_GRACE,
  EMERGENCY_YELLOW,
  EMERGENCY_ALL_RED,
  EMERGENCY_PRIORITY,
  EMERGENCY_EXIT_YELLOW,
  EMERGENCY_EXIT_ALL_RED,
  EMERGENCY_INSPECTOR_HOLD
};

EmergencyState emergencyState = EMERGENCY_IDLE;

unsigned long emergencyStateStart = 0;
unsigned long lastEmergencyBlink = 0;

bool emergencyBlinkState = false;

// =====================================================
// EMERGENCY TIMINGS
// =====================================================

const unsigned long EMERGENCY_GRACE_TIME = 2000;
const unsigned long EMERGENCY_YELLOW_TIME = 3000;
const unsigned long EMERGENCY_ALL_RED_TIME = 500;
const unsigned long EMERGENCY_BLINK_INTERVAL = 50;


// =====================================================
// EMERGENCY GREEN-TIME MEASUREMENT
// =====================================================

// Records when Lane 1 emergency green actually begins
unsigned long emergencyGreenStartTime = 0;

// Stores the total actual emergency green duration in milliseconds
unsigned long emergencyGreenDurationMs = 0;

// Prevents accidentally starting or stopping the timer multiple times
bool emergencyGreenTimerRunning = false;


// =====================================================
// TEMPORARY EMERGENCY COMPENSATION
// =====================================================

// Extra green time allocated to each lane, in seconds.
// Index 0 = Lane 1
// Index 1 = Lane 2
// Index 2 = Lane 3

unsigned long emergencyCompensationSec[3] = {0, 0, 0};

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

// Saved state for emergency recovery
Phase savedPhase = LANE_1_GREEN;
String savedMode = "auto";
String savedCommand = "none";

// Latest control request received during an emergency
String deferredMode = "auto";
String deferredCommand = "none";

bool deferredControlPending = false;

// Track the last values received from Firebase separately
// from the currently active traffic-light mode.
String lastReceivedMode = "auto";
String lastReceivedCommand = "none";

unsigned long savedRemainingTime = 0;

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
void startPhase(Phase newPhase, bool applyAdaptive = true);
void updateTrafficLights();

bool fetchEmergencyData();
void updateEmergencyControl();
void startEmergencyTransition();
void applyEmergencyYellow();
void applyEmergencyPriority();
void applyEmergencyExit();
void updateEmergencyValidity();

void updateCongestionDetection();

bool loadAdaptiveConfiguration();

bool patchAdaptiveLane(
  int laneIndex,
  bool includeTiming
);

void updateAdaptiveGreenDuration(int laneIndex);

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

bool validatePins(LaneConfig candidate[]) {

  int pins[9] = {
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

  for (int i = 0; i < 9; i++) {

    if (!isUsableOutputPin(pins[i])) {
      Serial.printf("Invalid GPIO: %d\n", pins[i]);
      return false;
    }

    for (int j = i + 1; j < 9; j++) {

      if (pins[i] == pins[j]) {
        Serial.printf("Duplicate GPIO: %d\n", pins[i]);
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

bool fetchConfiguration() {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Cannot fetch configuration.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  Serial.println("\nFetching Firebase configuration...");

  if (!http.begin(client, FIREBASE_URL)) {
    Serial.println("HTTP connection failed.");
    return false;
  }

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {

    Serial.printf("Configuration HTTP error: %d\n", httpCode);

    http.end();
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
    allLightsOff();
  }

  for (int i = 0; i < 3; i++) {
    lanes[i] = candidate[i];
  }

  for (int i = 0; i < 3; i++) {

    pinMode(lanes[i].red.pin, OUTPUT);
    pinMode(lanes[i].yellow.pin, OUTPUT);
    pinMode(lanes[i].green.pin, OUTPUT);
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
      "         RED %lu sec | YELLOW %lu sec | GREEN %lu sec\n",
      lanes[i].red.duration / 1000UL,
      lanes[i].yellow.duration / 1000UL,
      lanes[i].green.duration / 1000UL
    );
  }

  return true;
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


void calculateEmergencyCompensation() {

  // Clear any previous allocation.
  for (int i = 0; i < 3; i++) {
    emergencyCompensationSec[i] = 0;
  }

  // Convert measured emergency green time into whole seconds.
  unsigned long totalSeconds =
    emergencyGreenDurationMs / 1000UL;

  if (totalSeconds == 0) {
    Serial.println("No emergency green-time compensation required.");
    return;
  }

  // Get the current adaptive base durations.
  unsigned long lane2Base =
    adaptiveConfigLoaded
      ? adaptiveGreenDurationSec[1]
      : lanes[1].green.duration / 1000UL;

  unsigned long lane3Base =
    adaptiveConfigLoaded
      ? adaptiveGreenDurationSec[2]
      : lanes[2].green.duration / 1000UL;

  // Calculate available capacity before maximum green limits.
  unsigned long lane2Capacity =
    maxGreenDurationSec[1] > lane2Base
      ? maxGreenDurationSec[1] - lane2Base
      : 0;

  unsigned long lane3Capacity =
    maxGreenDurationSec[2] > lane3Base
      ? maxGreenDurationSec[2] - lane3Base
      : 0;

  // Initially divide compensation equally.
  unsigned long lane2Share = totalSeconds / 2;
  unsigned long lane3Share = totalSeconds - lane2Share;

  // Allocate within each lane's available capacity.
  unsigned long lane2Allocated =
    min(lane2Share, lane2Capacity);

  unsigned long lane3Allocated =
    min(lane3Share, lane3Capacity);

  emergencyCompensationSec[1] = lane2Allocated;
  emergencyCompensationSec[2] = lane3Allocated;

  // Calculate any compensation that could not initially fit.
  unsigned long remaining =
    totalSeconds - lane2Allocated - lane3Allocated;

  // First try to give the remaining time to Lane 2.
  unsigned long lane2RemainingCapacity =
    lane2Capacity - lane2Allocated;

  unsigned long extraLane2 =
    min(remaining, lane2RemainingCapacity);

  emergencyCompensationSec[1] += extraLane2;
  remaining -= extraLane2;

  // Then try Lane 3.
  unsigned long lane3RemainingCapacity =
    lane3Capacity - lane3Allocated;

  unsigned long extraLane3 =
    min(remaining, lane3RemainingCapacity);

  emergencyCompensationSec[2] += extraLane3;
  remaining -= extraLane3;

  Serial.println("\n========== EMERGENCY COMPENSATION ==========");

  Serial.printf(
    "Total emergency green: %lu seconds\n",
    totalSeconds
  );

  Serial.printf(
    "Lane 2 compensation: %lu seconds\n",
    emergencyCompensationSec[1]
  );

  Serial.printf(
    "Lane 3 compensation: %lu seconds\n",
    emergencyCompensationSec[2]
  );

  if (remaining > 0) {
    Serial.printf(
      "Unused compensation (maximum limits reached): %lu seconds\n",
      remaining
    );
  }

  Serial.println("=============================================");
}


unsigned long applyEmergencyCompensation(
  int laneIndex,
  unsigned long baseDurationMs
) {

  if (laneIndex < 0 || laneIndex >= 3) {
    return baseDurationMs;
  }

  unsigned long extraSeconds =
    emergencyCompensationSec[laneIndex];

  if (extraSeconds == 0) {
    return baseDurationMs;
  }

  unsigned long baseSeconds =
    baseDurationMs / 1000UL;

  unsigned long maximumSeconds =
    maxGreenDurationSec[laneIndex];

  unsigned long availableSeconds =
    maximumSeconds > baseSeconds
      ? maximumSeconds - baseSeconds
      : 0;

  // Never exceed the lane's maximum green duration.
  unsigned long appliedSeconds =
    min(extraSeconds, availableSeconds);

  unsigned long finalDurationMs =
    baseDurationMs + appliedSeconds * 1000UL;

  Serial.printf(
    "Lane %d: Applying %lu seconds of emergency compensation.\n",
    laneIndex + 1,
    appliedSeconds
  );

  // Clear this lane's compensation after its upcoming turn.
  emergencyCompensationSec[laneIndex] = 0;

  return finalDurationMs;
}


void startPhase(Phase newPhase, bool applyAdaptive) {

  if (!configLoaded) {
    Serial.println("Cannot start phase: configuration not loaded.");
    return;
  }

  // Reset all lanes before applying the new phase.
  setAllRed();
  currentPhase = newPhase;

  switch (currentPhase) {

    case LANE_1_GREEN:

      if (applyAdaptive) {
        updateAdaptiveGreenDuration(0);
      }

      digitalWrite(lanes[0].red.pin, LOW);
      digitalWrite(lanes[0].yellow.pin, LOW);
      digitalWrite(lanes[0].green.pin, HIGH);

      phaseDuration = adaptiveConfigLoaded
        ? adaptiveGreenDurationSec[0] * 1000UL
        : lanes[0].green.duration;

      phaseDuration =
        applyEmergencyCompensation(0, phaseDuration);

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

      if (applyAdaptive) {
        updateAdaptiveGreenDuration(1);
      }

      digitalWrite(lanes[1].red.pin, LOW);
      digitalWrite(lanes[1].yellow.pin, LOW);
      digitalWrite(lanes[1].green.pin, HIGH);

      phaseDuration = adaptiveConfigLoaded
        ? adaptiveGreenDurationSec[1] * 1000UL
        : lanes[1].green.duration;

      phaseDuration =
        applyEmergencyCompensation(1, phaseDuration);

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

      if (applyAdaptive) {
        updateAdaptiveGreenDuration(2);
      }

      digitalWrite(lanes[2].red.pin, LOW);
      digitalWrite(lanes[2].yellow.pin, LOW);
      digitalWrite(lanes[2].green.pin, HIGH);

      phaseDuration = adaptiveConfigLoaded
        ? adaptiveGreenDurationSec[2] * 1000UL
        : lanes[2].green.duration;

      phaseDuration =
        applyEmergencyCompensation(2, phaseDuration);

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

  // Make sure we have a valid Firebase authentication token.
  if (!ensureFirebaseAuth()) {
    Serial.println("Control fetch: Firebase authentication failed.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  // Attach Firebase authentication token.
  String authenticatedControlURL =
    String(CONTROL_URL) + "?auth=" + firebaseIdToken;

  if (!http.begin(client, authenticatedControlURL)) {
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

    // Force re-authentication if the token is rejected.
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


  // =====================================================
  // CHECK WHETHER A NEW CONTROL COMMAND WAS RECEIVED
  // =====================================================

  if (!controlInitialized ||
      receivedCommandId != lastCommandId ||
      receivedMode != lastReceivedMode ||
      receivedCommand != lastReceivedCommand) {

    controlInitialized = true;

    lastCommandId = receivedCommandId;
    lastReceivedMode = receivedMode;
    lastReceivedCommand = receivedCommand;

    Serial.println("\n========== NEW CONTROL STATE ==========");

    Serial.print("Received Mode: ");
    Serial.println(receivedMode);

    Serial.print("Received Command: ");
    Serial.println(receivedCommand);


    // =====================================================
    // CASE 1: EMERGENCY IS ACTIVE
    // =====================================================

    if (emergencyState != EMERGENCY_IDLE ||
        validEmergencyActive) {

      // If emergency is detected but the state machine
      // hasn't started yet, capture the current state first.
      if (emergencyState == EMERGENCY_IDLE &&
          validEmergencyActive) {

        startEmergencyTransition();
      }

      // Save the latest inspector request.
      deferredMode = receivedMode;
      deferredCommand = receivedCommand;
      deferredControlPending = true;

      Serial.println("Emergency active: control command deferred.");


      // ===================================================
      // CASE 1A: INSPECTOR SELECTED ALL YELLOW
      // ===================================================

      if (receivedMode == "manual" &&
          receivedCommand == "all_yellow") {

        Serial.println("Inspector selected ALL YELLOW.");
        Serial.println("Entering INSPECTOR HOLD.");

        // Update the active command so that the existing
        // manual command function applies all-yellow.
        currentMode = receivedMode;
        currentCommand = receivedCommand;

        applyManualCommand();

        emergencyState = EMERGENCY_INSPECTOR_HOLD;
        emergencyStateStart = millis();

        statusPublishPending = true;
      }


      // ===================================================
      // CASE 1B: INSPECTOR CHANGED AWAY FROM ALL YELLOW
      // ===================================================

      else if (emergencyState == EMERGENCY_INSPECTOR_HOLD) {

        if (validEmergencyActive) {

          Serial.println(
            "Inspector released ALL YELLOW. "
            "Starting emergency transition."
          );

          currentMode = receivedMode;
          currentCommand = receivedCommand;

          // Update the operating state that will be used
          // for the emergency yellow transition.
          savedMode = receivedMode;
          savedCommand = receivedCommand;
          emergencyState = EMERGENCY_GRACE;
          emergencyStateStart = millis();
        }
        else {

          Serial.println(
            "Emergency no longer valid. "
            "Applying the new manual/automatic command."
          );

          emergencyState = EMERGENCY_IDLE;

          currentMode = receivedMode;
          currentCommand = receivedCommand;

          deferredControlPending = false;

          if (currentMode == "manual") {
            applyManualCommand();
          }
          else if (currentMode == "auto") {
            startPhase(LANE_1_GREEN);
          }
          else {
            setAllRed();
          }
        }
      }

      // Ordinary commands remain deferred.
      // Do not change the physical traffic-light outputs.

      statusPublishPending = true;
    }


    // =====================================================
    // CASE 2: NO EMERGENCY IS ACTIVE
    // =====================================================

    else {

      currentMode = receivedMode;
      currentCommand = receivedCommand;

      deferredControlPending = false;

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
    }

    Serial.println("========================================");
  }

  return true;
}


// =====================================================
// EMERGENCY VEHICLE DATA FROM FIREBASE
// =====================================================

bool fetchEmergencyData() {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot fetch emergency data: WiFi disconnected.");
    return false;
  }

  if (!ensureFirebaseAuth()) {
    Serial.println("Emergency fetch: Firebase authentication failed.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String url =
    String(EMERGENCY_URL) +
    "?auth=" + firebaseIdToken;

  if (!http.begin(client, url)) {
    Serial.println("Emergency Firebase connection failed.");
    return false;
  }

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {

    Serial.printf(
      "Emergency Firebase GET failed. HTTP: %d\n",
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

  if (error) {
    Serial.println("Emergency JSON parsing failed.");
    Serial.println(error.c_str());
    return false;
  }

  // =========================================
  // 1. READ EMERGENCY STATUS
  // =========================================

  emergencyDetected =
    doc["emergency_vehicle_detected"] | false;

  // =========================================
  // 2. READ PRIORITY LANE
  // =========================================

  emergencyPriorityLane = 0;

  JsonVariantConst priority = doc["priority_lane"];

  if (priority.is<int>()) {

    emergencyPriorityLane = priority.as<int>();

  } else if (priority.is<const char*>()) {

    String lane = priority.as<String>();

    if (lane == "Lane_1" || lane == "lane_1") {
      emergencyPriorityLane = 1;
    }
    else if (lane == "Lane_2" || lane == "lane_2") {
      emergencyPriorityLane = 2;
    }
    else if (lane == "Lane_3" || lane == "lane_3") {
      emergencyPriorityLane = 3;
    }
  }

  // =========================================
  // 3. READ HEARTBEAT
  // =========================================

  emergencyLastHeartbeat =
    doc["last_heartbeat"] | 0ULL;

  // =========================================
  // 4. VALIDATE HEARTBEAT
  // =========================================

  time_t currentTime = time(nullptr);

  emergencyHeartbeatValid = false;
  emergencyHeartbeatAge = 0;

  if (currentTime > 1700000000 &&
      emergencyLastHeartbeat > 0) {

    uint64_t currentTimeMs =
      (uint64_t)currentTime * 1000ULL;

    if (currentTimeMs >= emergencyLastHeartbeat) {

      emergencyHeartbeatAge =
        currentTimeMs - emergencyLastHeartbeat;

      emergencyHeartbeatValid =
        emergencyHeartbeatAge <= EMERGENCY_HEARTBEAT_TIMEOUT;
    }
  }

  // =========================================
  // 5. PRINT EMERGENCY INFORMATION
  // =========================================

  Serial.println();
  Serial.println("========== EMERGENCY MONITOR ==========");

  Serial.printf(
    "Emergency detected: %s\n",
    emergencyDetected ? "YES" : "NO"
  );

  Serial.printf(
    "Priority lane: %d\n",
    emergencyPriorityLane
  );

  Serial.printf(
    "Last heartbeat: %llu\n",
    (unsigned long long)emergencyLastHeartbeat
  );

  if (currentTime <= 1700000000) {

    Serial.println("Heartbeat status: WAITING FOR CLOCK SYNC");

  } else if (emergencyLastHeartbeat == 0) {

    Serial.println("Heartbeat status: NOT RECEIVED");

  } else {

    Serial.printf(
      "Heartbeat age: %llu ms\n",
      (unsigned long long)emergencyHeartbeatAge
    );

    Serial.printf(
      "Heartbeat valid: %s\n",
      emergencyHeartbeatValid ? "YES" : "NO"
    );
  }

  // =========================================
  // 6. DETERMINE VALID EMERGENCY
  // =========================================

  validEmergencyActive =
  emergencyDetected &&
  emergencyHeartbeatValid &&
  emergencyPriorityLane == 1;

Serial.printf(
  "VALID EMERGENCY: %s\n",
  validEmergencyActive ? "YES" : "NO"
);

  Serial.println("=======================================");
  Serial.println();

  return true;
}


// =====================================================
// EMERGENCY TRAFFIC CONTROL
// =====================================================

// Recheck heartbeat freshness even between Firebase polls.
void updateEmergencyValidity() {

  emergencyHeartbeatValid = false;
  validEmergencyActive = false;

  if (emergencyLastHeartbeat == 0) {
    return;
  }

  time_t now;
  time(&now);

  if (now < 100000) {
    return;
  }

  uint64_t currentTimeMs = (uint64_t)now * 1000ULL;

  if (currentTimeMs < emergencyLastHeartbeat) {
    return;
  }

  emergencyHeartbeatAge =
    currentTimeMs - emergencyLastHeartbeat;

  emergencyHeartbeatValid =
    emergencyHeartbeatAge <= EMERGENCY_HEARTBEAT_TIMEOUT;

  // Only Lane 1 is allowed to receive emergency priority
  validEmergencyActive =
    emergencyDetected &&
    emergencyHeartbeatValid &&
    emergencyPriorityLane == 1;
}

// Start the transition into emergency mode.

void startEmergencyTransition() {

  Serial.println("\n========== EMERGENCY OVERRIDE ==========");

  // Save the normal operating state.
  savedPhase = currentPhase;
  savedMode = currentMode;
  savedCommand = currentCommand;

  savedRemainingTime = 0;

  // Save remaining automatic phase time.
  if (savedMode == "auto") {

    unsigned long elapsed = millis() - phaseStartTime;

    if (elapsed < phaseDuration) {
      savedRemainingTime = phaseDuration - elapsed;
    }
  }

  // Reset emergency green-time measurement.
  emergencyGreenStartTime = 0;
  emergencyGreenDurationMs = 0;
  emergencyGreenTimerRunning = false;

  // Reset temporary compensation.
  for (int i = 0; i < 3; i++) {
    emergencyCompensationSec[i] = 0;
  }

  Serial.println("Emergency detected!");
  Serial.printf("Priority Lane: %d\n", emergencyPriorityLane);

  // =====================================================
  // MANUAL ALL YELLOW: INSPECTOR HOLD
  // =====================================================

  if (savedMode == "manual" &&
      savedCommand == "all_yellow") {

    Serial.println("Inspector All Yellow detected.");
    Serial.println("Keeping all lanes YELLOW.");

    emergencyState = EMERGENCY_INSPECTOR_HOLD;
    emergencyStateStart = millis();

    // Preserve the inspector's existing light command.
    applyManualCommand();

    statusPublishPending = true;

    return;
  }

  // =====================================================
  // NORMAL EMERGENCY TRANSITION
  // =====================================================

  emergencyState = EMERGENCY_GRACE;
  emergencyStateStart = millis();

  Serial.println("Starting 2-second grace period.");
}

// Apply yellow to the currently active lane(s).

void applyEmergencyYellow() {

  // Begin with every lane RED.
  setAllRed();

  // =====================================================
  // MANUAL MODE
  // =====================================================

  if (savedMode == "manual") {

    if (savedCommand == "lane_1") {

      digitalWrite(lanes[0].red.pin, LOW);
      digitalWrite(lanes[0].yellow.pin, HIGH);
    }

    else if (savedCommand == "lane_2") {

      digitalWrite(lanes[1].red.pin, LOW);
      digitalWrite(lanes[1].yellow.pin, HIGH);
    }

    else if (savedCommand == "lane_3") {

      digitalWrite(lanes[2].red.pin, LOW);
      digitalWrite(lanes[2].yellow.pin, HIGH);
    }

    // Manual All Red remains All Red.
    // Manual All Yellow is handled by INSPECTOR_HOLD.

    return;
  }

  // =====================================================
  // AUTOMATIC MODE
  // =====================================================

  switch (savedPhase) {

    case LANE_1_GREEN:

      digitalWrite(lanes[0].red.pin, LOW);
      digitalWrite(lanes[0].yellow.pin, HIGH);

      break;

    case LANE_2_GREEN:

      digitalWrite(lanes[1].red.pin, LOW);
      digitalWrite(lanes[1].yellow.pin, HIGH);

      break;

    case LANE_3_GREEN:

      digitalWrite(lanes[2].red.pin, LOW);
      digitalWrite(lanes[2].yellow.pin, HIGH);

      break;

    case LANE_1_LANE_2_YELLOW:

      digitalWrite(lanes[0].red.pin, LOW);
      digitalWrite(lanes[0].yellow.pin, HIGH);

      digitalWrite(lanes[1].red.pin, LOW);
      digitalWrite(lanes[1].yellow.pin, HIGH);

      break;

    case LANE_2_LANE_3_YELLOW:

      digitalWrite(lanes[1].red.pin, LOW);
      digitalWrite(lanes[1].yellow.pin, HIGH);

      digitalWrite(lanes[2].red.pin, LOW);
      digitalWrite(lanes[2].yellow.pin, HIGH);

      break;

    case LANE_3_LANE_1_YELLOW:

      digitalWrite(lanes[2].red.pin, LOW);
      digitalWrite(lanes[2].yellow.pin, HIGH);

      digitalWrite(lanes[0].red.pin, LOW);
      digitalWrite(lanes[0].yellow.pin, HIGH);

      break;
  }
}

// Priority lane stays green.
// All other lanes blink red.

void applyEmergencyPriority() {

  if (!configLoaded) {
    return;
  }

  // Emergency priority is restricted to Lane 1.
  if (emergencyPriorityLane != 1) {
    setAllRed();
    return;
  }

  // =====================================================
  // LANE 1: GREEN
  // LANES 2 AND 3: BLINKING RED
  // =====================================================

  // Lane 1 gets GREEN.
  digitalWrite(lanes[0].red.pin, LOW);
  digitalWrite(lanes[0].yellow.pin, LOW);
  digitalWrite(lanes[0].green.pin, HIGH);

  // Lane 2 and Lane 3 get blinking RED.
  for (int i = 1; i < 3; i++) {

    digitalWrite(lanes[i].yellow.pin, LOW);
    digitalWrite(lanes[i].green.pin, LOW);

    digitalWrite(
      lanes[i].red.pin,
      emergencyBlinkState ? HIGH : LOW
    );
  }
}



// Start recovery after the emergency has ended.

void applyEmergencyExit() {

  // Start with all lanes RED.
  setAllRed();

  // Normal Lane 1 -> Lane 2 yellow transition.
  digitalWrite(lanes[0].red.pin, LOW);
  digitalWrite(lanes[0].yellow.pin, HIGH);

  digitalWrite(lanes[1].red.pin, LOW);
  digitalWrite(lanes[1].yellow.pin, HIGH);
}

void updateEmergencyControl() {

  if (!configLoaded) return;

  unsigned long now = millis();

  // Always refresh heartbeat validity using the cached heartbeat.
  updateEmergencyValidity();


  // =====================================================
  // 1. START A NEW EMERGENCY
  // =====================================================

  if (emergencyState == EMERGENCY_IDLE &&
      validEmergencyActive) {

    startEmergencyTransition();
  }


  // =====================================================
  // 2. HANDLE INSPECTOR ALL-YELLOW HOLD
  // =====================================================

  if (emergencyState == EMERGENCY_INSPECTOR_HOLD) {

    // Keep all lanes yellow while the inspector holds control.
    applyManualCommand();

    // If the emergency ends, restore the inspector's All Yellow.
    if (!validEmergencyActive) {

      Serial.println("Emergency ended during Inspector Hold.");

      emergencyState = EMERGENCY_IDLE;

      currentMode = "manual";
      currentCommand = "all_yellow";

      deferredControlPending = false;

      applyManualCommand();

      statusPublishPending = true;
    }

    return;
  }


  // =====================================================
  // 3. HANDLE EMERGENCY DISAPPEARING DURING GRACE
  // =====================================================

  if (emergencyState == EMERGENCY_GRACE &&
      !validEmergencyActive) {

    Serial.println("Emergency expired during grace period.");
    Serial.println("Cancelling emergency transition.");

    emergencyState = EMERGENCY_IDLE;

    // Restore the latest requested control state, if any.
    if (deferredControlPending) {

      currentMode = deferredMode;
      currentCommand = deferredCommand;

      deferredControlPending = false;

      if (currentMode == "manual") {
        applyManualCommand();
      }
      else if (currentMode == "auto") {
        startPhase(savedPhase, false);

        if (savedRemainingTime > 0) {
          phaseDuration = savedRemainingTime;
          phaseStartTime = millis();
        }
      }
      else {
        setAllRed();
      }
    }

    // Otherwise restore the original operating state.
    else {

      currentMode = savedMode;
      currentCommand = savedCommand;

      if (currentMode == "auto") {

        startPhase(savedPhase, false);

        if (savedRemainingTime > 0) {
          phaseDuration = savedRemainingTime;
          phaseStartTime = millis();
        }
      }
      else {
        applyManualCommand();
      }
    }

    statusPublishPending = true;

    return;
  }


  // =====================================================
  // 4. EMERGENCY ENDS DURING YELLOW OR PRIORITY
  // =====================================================

  if (emergencyState == EMERGENCY_PRIORITY &&
    !validEmergencyActive) {

    Serial.println("Emergency ended. Starting safe exit.");

    // Stop measuring Lane 1 emergency green time.
    if (emergencyGreenTimerRunning) {

      emergencyGreenDurationMs +=
        millis() - emergencyGreenStartTime;

      emergencyGreenTimerRunning = false;
      emergencyGreenStartTime = 0;

      Serial.printf(
        "Emergency green duration: %lu seconds\n",
        emergencyGreenDurationMs / 1000UL
      );
    }

    emergencyState = EMERGENCY_EXIT_YELLOW;
    emergencyStateStart = now;

    statusPublishPending = true;
  }


  switch (emergencyState) {

    // ===================================================
    // IDLE
    // ===================================================

    case EMERGENCY_IDLE:

      // Normal traffic controller retains control.
      break;


    // ===================================================
    // GRACE PERIOD
    // ===================================================

    case EMERGENCY_GRACE:

      // Keep existing lights unchanged during grace.
      if (now - emergencyStateStart >= EMERGENCY_GRACE_TIME) {

        // If we arrived here after Inspector Hold,
        // use the latest saved operating mode.
        if (savedMode == "manual" &&
            savedCommand == "all_yellow") {

          savedMode = currentMode;
          savedCommand = currentCommand;
        }

        Serial.println("Emergency transition: YELLOW");

        applyEmergencyYellow();

        emergencyState = EMERGENCY_YELLOW;
        emergencyStateStart = now;

        statusPublishPending = true;
      }

      break;


    // ===================================================
    // YELLOW TRANSITION
    // ===================================================

    case EMERGENCY_YELLOW:

      applyEmergencyYellow();

      if (now - emergencyStateStart >= EMERGENCY_YELLOW_TIME) {

        Serial.println("Emergency transition: ALL RED");

        setAllRed();

        emergencyState = EMERGENCY_ALL_RED;
        emergencyStateStart = now;

        statusPublishPending = true;
      }

      break;


    // ===================================================
    // ALL-RED CLEARANCE BEFORE EMERGENCY GREEN
    // ===================================================

    case EMERGENCY_ALL_RED:

      setAllRed();

      if (now - emergencyStateStart >= EMERGENCY_ALL_RED_TIME) {

        // CRITICAL: Recheck the heartbeat immediately
        // before granting emergency green.
        updateEmergencyValidity();

        if (!validEmergencyActive ||
            emergencyPriorityLane != 1) {

          Serial.println(
            "Emergency no longer valid. "
            "Lane 1 green will NOT be granted."
          );

          emergencyState = EMERGENCY_IDLE;

          // Restore the latest requested control state.
          if (deferredControlPending) {

            currentMode = deferredMode;
            currentCommand = deferredCommand;

            deferredControlPending = false;
          }
          else {

            currentMode = savedMode;
            currentCommand = savedCommand;
          }

          if (currentMode == "auto") {

            startPhase(savedPhase, false);

            if (savedRemainingTime > 0) {
              phaseDuration = savedRemainingTime;
              phaseStartTime = millis();
            }
          }
          else {

            applyManualCommand();
          }

          statusPublishPending = true;

          break;
        }

        Serial.println("EMERGENCY PRIORITY ACTIVATED!");

        emergencyBlinkState = true;
        lastEmergencyBlink = now;

        // Start green-time measurement exactly once.
        emergencyGreenStartTime = millis();
        emergencyGreenTimerRunning = true;

        emergencyState = EMERGENCY_PRIORITY;

        applyEmergencyPriority();

        statusPublishPending = true;
      }

      break;


    // ===================================================
    // EMERGENCY PRIORITY
    // ===================================================

    case EMERGENCY_PRIORITY:

      // Continue checking emergency validity.
      if (!validEmergencyActive) {
        break;
      }

      // Blink non-priority red lights.
      if (now - lastEmergencyBlink >= EMERGENCY_BLINK_INTERVAL) {

        lastEmergencyBlink = now;
        emergencyBlinkState = !emergencyBlinkState;
      }

      // Maintain emergency outputs.
      applyEmergencyPriority();

      break;


    // ===================================================
    // EMERGENCY EXIT: LANE 1 YELLOW
    // ===================================================

        // ===================================================
    // EMERGENCY EXIT: LANE 1 + LANE 2 YELLOW
    // ===================================================

    case EMERGENCY_EXIT_YELLOW:

      applyEmergencyExit();

      if (now - emergencyStateStart >= EMERGENCY_YELLOW_TIME) {

        // Recheck emergency status before resuming normal traffic.
        updateEmergencyValidity();

        if (validEmergencyActive &&
            emergencyPriorityLane == 1) {

          Serial.println(
            "Emergency still active. Restarting emergency transition."
          );

          emergencyState = EMERGENCY_GRACE;
          emergencyStateStart = millis();

          statusPublishPending = true;

          break;
        }

        Serial.println("Emergency recovery complete.");

        emergencyState = EMERGENCY_IDLE;

        // Restore the latest inspector request, if any.
        if (deferredControlPending) {

          currentMode = deferredMode;
          currentCommand = deferredCommand;

          deferredControlPending = false;
        }

        else {

          currentMode = savedMode;
          currentCommand = savedCommand;
        }

        // ===============================================
        // AUTOMATIC RECOVERY: LANE 2 GREEN
        // ===============================================

        if (currentMode == "auto") {

          Serial.println(
            "Resuming automatic traffic at Lane 2."
          );

          calculateEmergencyCompensation();

          startPhase(LANE_2_GREEN, false);
        }

        // ===============================================
        // MANUAL RECOVERY
        // ===============================================

        else if (currentMode == "manual") {

          Serial.println(
            "Restoring the latest manual command."
          );

          applyManualCommand();
        }

        else {

          setAllRed();
        }

        statusPublishPending = true;
      }

      break;

    // ===================================================
    // EMERGENCY EXIT: ALL RED
    // ===================================================

    case EMERGENCY_EXIT_ALL_RED:

      setAllRed();

      if (now - emergencyStateStart >= EMERGENCY_ALL_RED_TIME) {

        // Revalidate in case another emergency arrived
        // while the exit sequence was running.
        updateEmergencyValidity();

        if (validEmergencyActive &&
            emergencyPriorityLane == 1) {

          Serial.println(
            "Emergency still active. "
            "Restarting emergency transition."
          );

          emergencyState = EMERGENCY_GRACE;
          emergencyStateStart = millis();

          statusPublishPending = true;

          break;
        }

        Serial.println("Emergency recovery complete.");

        emergencyState = EMERGENCY_IDLE;

        // Restore the latest inspector request.
        if (deferredControlPending) {

          currentMode = deferredMode;
          currentCommand = deferredCommand;

          deferredControlPending = false;
        }
        else {

          currentMode = savedMode;
          currentCommand = savedCommand;
        }

        // Automatic recovery ALWAYS starts at Lane 2.
        if (currentMode == "auto") {

          Serial.println(
            "Resuming automatic traffic at Lane 2."
          );

          // Calculate compensation before restarting automatic traffic.
          calculateEmergencyCompensation();

          startPhase(LANE_2_GREEN, false);
        }

        // Manual recovery restores the latest command.
        else if (currentMode == "manual") {

          Serial.println(
            "Restoring the latest manual command."
          );

          applyManualCommand();
        }

        else {

          setAllRed();
        }

        statusPublishPending = true;
      }

      break;


    // ===================================================
    // INSPECTOR HOLD
    // ===================================================

    case EMERGENCY_INSPECTOR_HOLD:

      // Already handled before the switch.
      break;
  }
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
  doc["lastUpdated"] = millis();

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
// PATCH ADAPTIVE TRAFFIC DATA TO FIREBASE
// =====================================================

bool patchAdaptiveLane(
  int laneIndex,
  bool includeTiming
) {

  if (laneIndex < 0 || laneIndex >= 3) {
    return false;
  }

  if (!ensureFirebaseAuth()) {
    Serial.println("Adaptive Firebase authentication failed.");
    return false;
  }

  String laneURL =
    String(ADAPTIVE_BASE_URL) +
    "/Lane_" +
    String(laneIndex + 1) +
    ".json?auth=" +
    firebaseIdToken;

  JsonDocument doc;

  doc["congested"] = laneCongested[laneIndex];

  if (includeTiming) {

    doc["greenDuration"] =
      adaptiveGreenDurationSec[laneIndex];

    doc["originalGreenDuration"] =
      originalGreenDurationSec[laneIndex];

    doc["maxGreenDuration"] =
      maxGreenDurationSec[laneIndex];

    doc["incrementSeconds"] =
      incrementSeconds[laneIndex];

    doc["lastDecision"] =
      adaptiveLastDecision[laneIndex];
  }

  String payload;
  serializeJson(doc, payload);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  http.setConnectTimeout(3000);
  http.setTimeout(3000);

  if (!http.begin(client, laneURL)) {
    Serial.println("Adaptive Firebase connection failed.");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  int httpCode = http.sendRequest("PATCH", payload);

  String response = http.getString();

  http.end();

  if (httpCode == HTTP_CODE_OK) {

    Serial.printf(
      "Lane %d adaptive data updated in Firebase.\n",
      laneIndex + 1
    );

    return true;
  }

  Serial.printf(
    "Lane %d adaptive update failed. HTTP: %d\n",
    laneIndex + 1,
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
// LOAD ADAPTIVE SETTINGS FROM FIREBASE
// =====================================================

bool loadAdaptiveConfiguration() {

  if (!ensureFirebaseAuth()) {
    Serial.println("Cannot load adaptive configuration: authentication failed.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String url =
    String(ADAPTIVE_BASE_URL) +
    ".json?auth=" +
    firebaseIdToken;

  http.setConnectTimeout(3000);
  http.setTimeout(3000);

  if (!http.begin(client, url)) {
    Serial.println("Adaptive configuration connection failed.");
    return false;
  }

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {

    Serial.printf(
      "Adaptive configuration fetch failed: %d\n",
      httpCode
    );

    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  JsonDocument doc;

  if (deserializeJson(doc, response)) {
    Serial.println("Adaptive configuration JSON parsing failed.");
    return false;
  }

  Serial.println("\nLoading adaptive traffic settings...");

  for (int i = 0; i < 3; i++) {

    JsonVariantConst laneData =
      doc[i == 0 ? "Lane_1" :
          i == 1 ? "Lane_2" : "Lane_3"];

    // Start with the original trafficLight configuration.
    originalGreenDurationSec[i] =
      lanes[i].green.duration / 1000UL;

    adaptiveGreenDurationSec[i] =
      originalGreenDurationSec[i];

    maxGreenDurationSec[i] = 60;
    incrementSeconds[i] = 5;

    adaptiveLastDecision[i] = "normal";

    if (!laneData.isNull()) {

      if (laneData["originalGreenDuration"].is<unsigned long>()) {
        originalGreenDurationSec[i] =
          laneData["originalGreenDuration"].as<unsigned long>();
      }

      if (laneData["maxGreenDuration"].is<unsigned long>()) {
        maxGreenDurationSec[i] =
          laneData["maxGreenDuration"].as<unsigned long>();
      }

      if (laneData["incrementSeconds"].is<unsigned long>()) {
        incrementSeconds[i] =
          laneData["incrementSeconds"].as<unsigned long>();
      }

      if (laneData["greenDuration"].is<unsigned long>()) {
        adaptiveGreenDurationSec[i] =
          laneData["greenDuration"].as<unsigned long>();
      }

      if (laneData["lastDecision"].is<const char*>()) {
        adaptiveLastDecision[i] =
          laneData["lastDecision"].as<String>();
      }
    }

    // Validate the adaptive settings.
    if (maxGreenDurationSec[i] < originalGreenDurationSec[i]) {
      maxGreenDurationSec[i] = originalGreenDurationSec[i];
    }

    if (incrementSeconds[i] == 0) {
      incrementSeconds[i] = 5;
    }

    if (adaptiveGreenDurationSec[i] < originalGreenDurationSec[i]) {
      adaptiveGreenDurationSec[i] = originalGreenDurationSec[i];
    }

    if (adaptiveGreenDurationSec[i] > maxGreenDurationSec[i]) {
      adaptiveGreenDurationSec[i] = maxGreenDurationSec[i];
    }

    Serial.printf(
      "Lane %d | Original: %lu sec | Current: %lu sec | Maximum: %lu sec | Increment: %lu sec\n",
      i + 1,
      originalGreenDurationSec[i],
      adaptiveGreenDurationSec[i],
      maxGreenDurationSec[i],
      incrementSeconds[i]
    );

  }

  // Sync the initialized values to Firebase.
  for (int i = 0; i < 3; i++) {

    if (!patchAdaptiveLane(i, true)) {
      Serial.println("Could not initialize all adaptive Firebase values.");
      return false;
    }
  }

  adaptiveConfigLoaded = true;

  Serial.println("Adaptive configuration loaded successfully!");

  return true;
}

// =====================================================
// ADJUST ADAPTIVE GREEN DURATION
// =====================================================

void updateAdaptiveGreenDuration(int laneIndex) {

  if (!adaptiveConfigLoaded ||
      laneIndex < 0 ||
      laneIndex >= 3) {
    return;
  }

  // Take a fresh reading immediately before this lane turns GREEN.
  laneCongested[laneIndex] =
    (digitalRead(IR_PINS[laneIndex]) == LOW);

  vehicleDetected[laneIndex] = laneCongested[laneIndex];

  unsigned long previousDuration =
    adaptiveGreenDurationSec[laneIndex];

  unsigned long newDuration = previousDuration;

  if (laneCongested[laneIndex]) {

    newDuration = min(
      previousDuration + incrementSeconds[laneIndex],
      maxGreenDurationSec[laneIndex]
    );
  }

  else {

    if (previousDuration > originalGreenDurationSec[laneIndex]) {

      if (previousDuration - originalGreenDurationSec[laneIndex]
          <= incrementSeconds[laneIndex]) {

        newDuration = originalGreenDurationSec[laneIndex];
      }

      else {

        newDuration =
          previousDuration - incrementSeconds[laneIndex];
      }
    }
  }

  if (newDuration > previousDuration) {
    adaptiveLastDecision[laneIndex] = "increased";
  }

  else if (newDuration < previousDuration) {
    adaptiveLastDecision[laneIndex] = "decreased";
  }

  else {
    adaptiveLastDecision[laneIndex] = "normal";
  }

  if (newDuration == previousDuration &&
      laneCongested[laneIndex] &&
      previousDuration >= maxGreenDurationSec[laneIndex]) {

    adaptiveLastDecision[laneIndex] = "max_limit";
  }

  // Update local value temporarily.
  adaptiveGreenDurationSec[laneIndex] = newDuration;

  // Persist the new duration before applying it.
  if (!patchAdaptiveLane(laneIndex, true)) {

    // Keep physical timing and Firebase consistent.
    adaptiveGreenDurationSec[laneIndex] = previousDuration;

    Serial.println(
      "Adaptive timing not applied because Firebase update failed."
    );

    return;
  }

  Serial.println();
  Serial.printf(
    "========== LANE %d ADAPTIVE DECISION ==========\n",
    laneIndex + 1
  );

  Serial.printf(
    "Congestion: %s\n",
    laneCongested[laneIndex] ? "YES" : "NO"
  );

  Serial.printf(
    "Previous duration: %lu seconds\n",
    previousDuration
  );

  Serial.printf(
    "New duration: %lu seconds\n",
    adaptiveGreenDurationSec[laneIndex]
  );

  Serial.printf(
    "Decision: %s\n",
    adaptiveLastDecision[laneIndex].c_str()
  );

  Serial.println("===============================================");
}

// =====================================================
// IR SENSOR STATUS MONITORING
// =====================================================

void updateCongestionDetection() {

  if (!configLoaded) {
    return;
  }

  for (int i = 0; i < 3; i++) {

    // ACTIVE LOW: LOW means a vehicle is detected.
    bool detected = (digitalRead(IR_PINS[i]) == LOW);

    vehicleDetected[i] = detected;
    laneCongested[i] = detected;
  }

  // Print sensor status once per second.
  if (millis() - lastSensorPrint >= SENSOR_PRINT_INTERVAL) {

    lastSensorPrint = millis();

    Serial.println();
    Serial.println("========== SENSOR STATUS ==========");

    for (int i = 0; i < 3; i++) {

      Serial.printf(
        "Lane %d | Raw: %s | Vehicle: %s | Congestion: %s | Green: %lu sec\n",
        i + 1,
        digitalRead(IR_PINS[i]) == LOW ? "LOW" : "HIGH",
        vehicleDetected[i] ? "YES" : "NO",
        laneCongested[i] ? "YES" : "NO",
        adaptiveGreenDurationSec[i]
      );
    }

    Serial.println("===================================");
  }
}

// =====================================================
// 24. SETUP
// =====================================================

void setup() {

  Serial.begin(115200);
  delay(300);

  // Configure IR sensors
for (int i = 0; i < 3; i++) {
  pinMode(IR_PINS[i], INPUT);
}

Serial.println("IR sensors initialized.");

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

// Synchronize clock for Firebase heartbeat validation.
configTime(
  0,
  0,
  "pool.ntp.org",
  "time.google.com"
);

Serial.println("Waiting for time synchronization...");

time_t now = time(nullptr);

unsigned long timeWaitStart = millis();

while (now < 1700000000 &&
       millis() - timeWaitStart < 10000) {

  delay(250);
  now = time(nullptr);
}

if (now > 1700000000) {
  Serial.println("Clock synchronized successfully!");
} else {
  Serial.println("WARNING: Clock synchronization failed.");
}

  // Firebase sign-in will occur when status is first published.

  if (fetchConfiguration()) {

    if (!loadAdaptiveConfiguration()) {
      Serial.println(
        "Adaptive configuration unavailable. Using original timings."
      );
    }

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

  // Continue the local emergency state machine
  // even when Firebase is temporarily unavailable.
  if (configLoaded) {
    updateEmergencyControl();
  }

  return;
}

  // Continuously refresh the live IR sensor status.
  updateCongestionDetection();

  // Retry adaptive settings if Firebase was temporarily unavailable at startup.
  if (configLoaded && !adaptiveConfigLoaded &&
      millis() - lastAdaptiveConfigAttempt >= ADAPTIVE_CONFIG_RETRY_INTERVAL) {

    lastAdaptiveConfigAttempt = millis();

    if (loadAdaptiveConfiguration()) {
      Serial.println("Adaptive configuration restored successfully.");
    }
  }

  // 2. Poll Firebase control.

  if (millis() - lastControlCheck >= CONTROL_CHECK_INTERVAL) {

    lastControlCheck = millis();

    fetchControlCommand();
  }

// 2.1 Poll emergency vehicle information.

if (
  millis() - lastEmergencyCheck >=
  EMERGENCY_CHECK_INTERVAL
) {

  lastEmergencyCheck = millis();

  fetchEmergencyData();
}

// 2.2 Handle emergency traffic override.
if (configLoaded) {
  updateEmergencyControl();
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

 
 if (configLoaded &&
    currentMode == "auto" &&
    emergencyState == EMERGENCY_IDLE) {

  updateTrafficLights();
}
  // 5. Publish live status.

  if (millis() - lastStatusPublish >= STATUS_PUBLISH_INTERVAL) {

    lastStatusPublish = millis();

    publishStatus();
  }
}