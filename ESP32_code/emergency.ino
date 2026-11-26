
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TinyGPSPlus.h>
#include <time.h>

// =====================================================
// 1. WIFI CONFIGURATION
// =====================================================

const char* WIFI_SSID = "";
const char* WIFI_PASSWORD = "";

// =====================================================
// 2. FIREBASE CONFIGURATION
// =====================================================

const char* FIREBASE_API_KEY = "";

const char* FIREBASE_DATABASE_URL =
  "https://traffic-light-86038-default-rtdb.asia-southeast1.firebasedatabase.app";

const char* FIREBASE_EMAIL = "";
const char* FIREBASE_PASSWORD = "";

String firebaseToken = "";
unsigned long tokenCreatedAt = 0;

// Refresh authentication before the token expires.
const unsigned long TOKEN_REFRESH_INTERVAL = 50UL * 60UL * 1000UL;

// =====================================================
// 3. GPS CONFIGURATION
// =====================================================

TinyGPSPlus gps;
HardwareSerial GPS_Serial(2);

const int GPS_RX_PIN = 16;
const int GPS_TX_PIN = 17;

const int GPS_BAUD_RATE = 9600;

// =====================================================
// 4. EMERGENCY BUTTON
// =====================================================

const int BUTTON_PIN = 27;

// Button uses INPUT_PULLUP.
// LOW = emergency button pressed.

bool buttonPressed = false;

// =====================================================
// 5. EMERGENCY CONFIGURATION
// =====================================================

const char* LANE_NAMES[3] = {
  "Lane_1",
  "Lane_2",
  "Lane_3"
};

const unsigned long HEARTBEAT_INTERVAL = 1000;

const unsigned long GPS_MAX_AGE = 3000;

const int MIN_GPS_SATELLITES = 4;

unsigned long lastHeartbeat = 0;

// =====================================================
// 6. GPS LOCATION VARIABLES
// =====================================================

double currentLatitude = 0.0;
double currentLongitude = 0.0;

bool gpsLocationValid = false;
bool locationAvailable = false;

String locationSource = "NONE";

unsigned long lastGpsUpdate = 0;

// =====================================================
// 7. FIREBASE AUTHENTICATION
// =====================================================

bool signInToFirebase() {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Cannot authenticate.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String url =
    "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key="
    + String(FIREBASE_API_KEY);

  if (!http.begin(client, url)) {
    Serial.println("Firebase authentication connection failed.");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument requestDoc(512);

  requestDoc["email"] = FIREBASE_EMAIL;
  requestDoc["password"] = FIREBASE_PASSWORD;
  requestDoc["returnSecureToken"] = true;

  String requestBody;
  serializeJson(requestDoc, requestBody);

  int httpCode = http.POST(requestBody);

  if (httpCode != 200) {

    Serial.print("Firebase authentication failed: ");
    Serial.println(httpCode);

    Serial.println(http.getString());

    http.end();
    return false;
  }

  String response = http.getString();

  DynamicJsonDocument responseDoc(2048);

  DeserializationError error =
    deserializeJson(responseDoc, response);

  if (error) {
    Serial.println("Authentication response parsing failed.");
    http.end();
    return false;
  }

  firebaseToken =
    responseDoc["idToken"].as<String>();

  tokenCreatedAt = millis();

  http.end();

  if (firebaseToken.length() == 0) {
    Serial.println("Firebase token is empty.");
    return false;
  }

  Serial.println("Firebase authentication successful.");

  return true;
}

// =====================================================
// 8. ENSURE FIREBASE AUTHENTICATION
// =====================================================

bool ensureFirebaseAuth() {

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (
    firebaseToken.length() == 0 ||
    millis() - tokenCreatedAt >= TOKEN_REFRESH_INTERVAL
  ) {
    return signInToFirebase();
  }

  return true;
}

// =====================================================
// 9. FIREBASE REST REQUEST
// =====================================================

bool firebaseRequest(
  const String& method,
  const String& path,
  const String& payload,
  String& response
) {

  if (!ensureFirebaseAuth()) {
    Serial.println("Firebase authentication unavailable.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String url =
    String(FIREBASE_DATABASE_URL) +
    "/" + path + ".json?auth=" + firebaseToken;

  if (!http.begin(client, url)) {
    Serial.println("Firebase HTTP connection failed.");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  int httpCode = http.sendRequest(
    method.c_str(),
    payload
  );

  response = http.getString();

  http.end();

  if (httpCode < 200 || httpCode >= 300) {

    Serial.print("Firebase request failed: ");
    Serial.println(httpCode);

    Serial.println(response);

    return false;
  }

  return true;
}

// =====================================================
// 10. GET CURRENT UNIX TIME IN MILLISECONDS
// =====================================================

unsigned long long getCurrentTimestamp() {

  time_t now = time(nullptr);

  if (now < 1700000000) {
    return 0;
  }

  return (unsigned long long)now * 1000ULL;
}

// =====================================================
// 11. CHECK GPS DATA
// =====================================================

void updateGPS() {

  while (GPS_Serial.available()) {
    gps.encode(GPS_Serial.read());
  }

  gpsLocationValid = false;

  if (
    gps.location.isValid() &&
    gps.location.age() <= GPS_MAX_AGE &&
    gps.satellites.isValid() &&
    gps.satellites.value() >= MIN_GPS_SATELLITES
  ) {

    currentLatitude = gps.location.lat();
    currentLongitude = gps.location.lng();

    gpsLocationValid = true;
    locationAvailable = true;

    locationSource = "GPS";

    lastGpsUpdate = millis();
  }
}

// =====================================================
// 12. LOAD SAVED FIREBASE LOCATION
// =====================================================

void loadFirebaseFallback(
  JsonDocument& root
) {

  if (gpsLocationValid) {
    return;
  }

  JsonVariant latitudeValue =
    root["latitude"];

  JsonVariant longitudeValue =
    root["longitude"];

  if (
    !latitudeValue.isNull() &&
    !longitudeValue.isNull()
  ) {

    double savedLatitude =
      latitudeValue.as<double>();

    double savedLongitude =
      longitudeValue.as<double>();

    if (
      savedLatitude >= -90.0 &&
      savedLatitude <= 90.0 &&
      savedLongitude >= -180.0 &&
      savedLongitude <= 180.0 &&
      !(savedLatitude == 0.0 && savedLongitude == 0.0)
    ) {

      currentLatitude = savedLatitude;
      currentLongitude = savedLongitude;

      locationAvailable = true;
      locationSource = "FIREBASE_FALLBACK";

      return;
    }
  }

  locationAvailable = false;
  locationSource = "NONE";
}

// =====================================================
// 13. UPDATE SAVED LOCATION IN FIREBASE
// =====================================================

void saveCurrentLocation() {

  if (!gpsLocationValid) {
    return;
  }

  DynamicJsonDocument doc(256);

  doc["latitude"] = currentLatitude;
  doc["longitude"] = currentLongitude;

  unsigned long long timestamp =
    getCurrentTimestamp();

  if (timestamp > 0) {
    doc["last_gps_update"] = timestamp;
  }

  String payload;
  serializeJson(doc, payload);

  String response;

  if (
    firebaseRequest(
      "PATCH",
      "emergency",
      payload,
      response
    )
  ) {

    Serial.println("Latest GPS location saved to Firebase.");
  }
}

// =====================================================
// 14. PROCESS ZONE UPDATE REQUESTS
// =====================================================

void processZoneUpdates(
  JsonDocument& root
) {

  if (!gpsLocationValid) {
    return;
  }

  for (int i = 0; i < 3; i++) {

    const char* laneName = LANE_NAMES[i];

    JsonObject zone =
      root["zones"][laneName];

    if (zone.isNull()) {
      continue;
    }

    bool updateRequested =
      zone["update"] | false;

    if (!updateRequested) {
      continue;
    }

    Serial.print("Updating coordinates for ");
    Serial.println(laneName);

    DynamicJsonDocument updateDoc(256);

    updateDoc["latitude"] = currentLatitude;
    updateDoc["longitude"] = currentLongitude;
    updateDoc["update"] = false;

    String payload;
    serializeJson(updateDoc, payload);

    String response;

    String path =
      String("emergency/zones/") + laneName;

    if (
      firebaseRequest(
        "PATCH",
        path,
        payload,
        response
      )
    ) {

      zone["latitude"] = currentLatitude;
      zone["longitude"] = currentLongitude;
      zone["update"] = false;

      Serial.print(laneName);
      Serial.println(" coordinates updated successfully.");
    }
    else {

      Serial.print("Could not update ");
      Serial.println(laneName);
    }
  }
}

// =====================================================
// 15. CHECK WHETHER VEHICLE IS INSIDE A ZONE
// =====================================================

String detectEmergencyLane(
  JsonDocument& root
) {

  if (!buttonPressed || !locationAvailable) {
    return "";
  }

  for (int i = 0; i < 3; i++) {

    const char* laneName = LANE_NAMES[i];

    JsonObject zone =
      root["zones"][laneName];

    if (zone.isNull()) {
      continue;
    }

    bool enabled =
      zone["enabled"] | false;

    if (!enabled) {
      continue;
    }

    double zoneLatitude =
      zone["latitude"] | 0.0;

    double zoneLongitude =
      zone["longitude"] | 0.0;

    double radius =
      zone["radius"] | 0.0;

    if (
      radius <= 0 ||
      (zoneLatitude == 0.0 && zoneLongitude == 0.0)
    ) {
      continue;
    }

    double distance =
      TinyGPSPlus::distanceBetween(
        currentLatitude,
        currentLongitude,
        zoneLatitude,
        zoneLongitude
      );

    Serial.print(laneName);
    Serial.print(" distance: ");
    Serial.print(distance);
    Serial.println(" metres");

    if (distance <= radius) {

      Serial.print("EMERGENCY ZONE DETECTED: ");
      Serial.println(laneName);

      return String(laneName);
    }
  }

  return "";
}

// =====================================================
// 16. PUBLISH EMERGENCY STATUS
// =====================================================

void publishEmergencyStatus(
  const String& priorityLane
) {

  bool emergencyActive =
    priorityLane.length() > 0;

  DynamicJsonDocument doc(512);

  doc["emergency_vehicle_detected"] =
    emergencyActive;

  if (emergencyActive) {
    doc["priority_lane"] = priorityLane;
  }
  else {
    doc["priority_lane"] = nullptr;
  }

  doc["location_source"] = locationSource;

  if (locationAvailable) {
    doc["latitude"] = currentLatitude;
    doc["longitude"] = currentLongitude;
  }

  unsigned long long timestamp =
    getCurrentTimestamp();

  if (timestamp > 0) {
    doc["last_heartbeat"] = timestamp;
  }

  String payload;
  serializeJson(doc, payload);

  String response;

  if (
    firebaseRequest(
      "PATCH",
      "emergency",
      payload,
      response
    )
  ) {

    Serial.println("-----------------------------");

    if (emergencyActive) {

      Serial.println("EMERGENCY ACTIVE");
      Serial.print("Priority lane: ");
      Serial.println(priorityLane);
    }
    else {

      Serial.println("NO ACTIVE EMERGENCY");
    }

    Serial.print("Location source: ");
    Serial.println(locationSource);

    Serial.println("-----------------------------");
  }
}

// =====================================================
// 17. READ EMERGENCY BUTTON
// =====================================================

void readEmergencyButton() {

  buttonPressed =
    (digitalRead(BUTTON_PIN) == LOW);
}

// =====================================================
// 18. CONNECT TO WIFI
// =====================================================

void connectWiFi() {

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  Serial.print("Connecting to WiFi");

  unsigned long startTime = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - startTime < 20000
  ) {

    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println("\nWiFi connected.");

    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  }
  else {

    Serial.println("\nWiFi connection failed.");
  }
}

// =====================================================
// 19. SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  pinMode(
    BUTTON_PIN,
    INPUT_PULLUP
  );

  GPS_Serial.begin(
    GPS_BAUD_RATE,
    SERIAL_8N1,
    GPS_RX_PIN,
    GPS_TX_PIN
  );

  Serial.println();
  Serial.println("================================");
  Serial.println("EMERGENCY VEHICLE CONTROLLER");
  Serial.println("================================");

  connectWiFi();

  // India Standard Time: UTC + 5:30
  configTime(
    19800,
    0,
    "pool.ntp.org",
    "time.google.com"
  );

  Serial.println("Waiting for network time...");

  unsigned long startTime = millis();

  while (
    time(nullptr) < 1700000000 &&
    millis() - startTime < 10000
  ) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    signInToFirebase();
  }

  Serial.println("Emergency controller ready.");
}

// =====================================================
// 20. MAIN LOOP
// =====================================================

void loop() {

  // Keep reading GPS continuously.
  updateGPS();

  // Read emergency button.
  readEmergencyButton();

  // Reconnect WiFi if disconnected.
  if (WiFi.status() != WL_CONNECTED) {

    static unsigned long lastReconnect = 0;

    if (millis() - lastReconnect >= 5000) {

      lastReconnect = millis();

      Serial.println("Reconnecting WiFi...");

      WiFi.disconnect();
      WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
      );
    }

    return;
  }

  // Publish heartbeat at the configured interval.
  if (
    millis() - lastHeartbeat >= HEARTBEAT_INTERVAL
  ) {

    lastHeartbeat = millis();

    String response;

    // Fetch current emergency configuration.
    if (
      !firebaseRequest(
        "GET",
        "emergency",
        "",
        response
      )
    ) {

      Serial.println("Could not fetch emergency configuration.");
      return;
    }

    DynamicJsonDocument root(4096);

    DeserializationError error =
      deserializeJson(root, response);

    if (error) {

      Serial.print("Firebase JSON error: ");
      Serial.println(error.c_str());

      return;
    }

    // Use GPS location if valid.
    // Otherwise use the last saved Firebase location.
    loadFirebaseFallback(root);

    // If GPS is fresh, update the saved location.
    if (gpsLocationValid) {

      saveCurrentLocation();

      // Update any zone whose update flag is true.
      processZoneUpdates(root);
    }

    // Check all enabled lane zones.
    String priorityLane =
      detectEmergencyLane(root);

    // Publish the emergency state and heartbeat.
    publishEmergencyStatus(priorityLane);
  }
}
