#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ThingSpeak.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <Servo.h>

char ssid[] = <Your_WiFi_name>;
char wifiPassword[] = <your_WiFi_password>;

const char* writeApiKey = <Thingspeak_write_api_key>;
const char* readApiKey =  <Thingspeak_write_api_key>;
const char* server = "api.thingspeak.com";
unsigned long channelID = <your_channel_id>;

unsigned long lastThingSpeakUpdate = 0;
const unsigned long thingSpeakInterval = 15000;
unsigned long lastSensorRead = 0;
const unsigned long sensorReadInterval = 200;
unsigned long gateOpenTime = 0;
const unsigned long gateOpenDuration = 5000;

WiFiClient client;
ESP8266WebServer localServer(80);

#define TRIG_PIN_1 D2
#define ECHO_PIN_1 D1
#define TRIG_PIN_2 D5
#define ECHO_PIN_2 D6
#define IR_SENSOR D0
#define LED_RESERVE_1 D7
#define LED_RESERVE_2 D8
#define LED_OCCUPIED_1 D3
#define LED_OCCUPIED_2 D4

Servo entryGate;
#define SERVO_PIN D9
#define GATE_CLOSED_POS 0
#define GATE_OPEN_POS 90

bool gateIsOpen = false;

bool slot1Occupied = false;
bool slot2Occupied = false;
bool slot1Reserved = false;
bool slot2Reserved = false;

bool slot1PrevOccupied = false;
bool slot2PrevOccupied = false;
bool slot1PrevReserved = false;
bool slot2PrevReserved = false;
bool stateChanged = false;

const int READING_SAMPLES = 3;
long slot1Distances[READING_SAMPLES];
long slot2Distances[READING_SAMPLES];
int sampleIndex = 0;

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN_1, OUTPUT);
  pinMode(ECHO_PIN_1, INPUT);
  pinMode(TRIG_PIN_2, OUTPUT);
  pinMode(ECHO_PIN_2, INPUT);
  pinMode(IR_SENSOR, INPUT);
  pinMode(LED_RESERVE_1, OUTPUT);
  pinMode(LED_RESERVE_2, OUTPUT);
  pinMode(LED_OCCUPIED_1, OUTPUT);
  pinMode(LED_OCCUPIED_2, OUTPUT);
  digitalWrite(LED_RESERVE_1, LOW);
  digitalWrite(LED_RESERVE_2, LOW);
  digitalWrite(LED_OCCUPIED_1, LOW);
  digitalWrite(LED_OCCUPIED_2, LOW);
  entryGate.attach(SERVO_PIN);
  entryGate.write(GATE_CLOSED_POS);
  for (int i = 0; i < READING_SAMPLES; i++) {
    slot1Distances[i] = -1;
    slot2Distances[i] = -1;
  }
  WiFi.begin(ssid, wifiPassword);
  Serial.print("Connecting to WiFi");
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 20) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to WiFi!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    ThingSpeak.begin(client);
    setupLocalServer();
    checkReservations();
  } else {
    Serial.println("\nFailed to connect to WiFi. Will try again later.");
  }
}

void setupLocalServer() {
  localServer.on("/status", HTTP_GET, []() {
    DynamicJsonDocument doc(256);
    doc["slot1"]["occupied"] = slot1Occupied;
    doc["slot1"]["reserved"] = slot1Reserved;
    doc["slot2"]["occupied"] = slot2Occupied;
    doc["slot2"]["reserved"] = slot2Reserved;
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    localServer.sendHeader("Access-Control-Allow-Origin", "*");
    localServer.send(200, "application/json", jsonResponse);
  });

  localServer.on("/reserve", HTTP_GET, []() {
    String slotStr = localServer.arg("slot");
    String statusStr = localServer.arg("status");
    int slot = slotStr.toInt();
    bool status = (statusStr == "1" || statusStr == "true");
    bool success = false;
    if (slot == 1) {
      slot1Reserved = status;
      digitalWrite(LED_RESERVE_1, status ? HIGH : LOW);
      success = true;
      if (status) {
        Serial.println("Slot 1 RESERVED");
      } else {
        Serial.println("Slot 1 RESERVATION CANCELLED");
      }
    } else if (slot == 2) {
      slot2Reserved = status;
      digitalWrite(LED_RESERVE_2, status ? HIGH : LOW);
      success = true;
      if (status) {
        Serial.println("Slot 2 RESERVED");
      } else {
        Serial.println("Slot 2 RESERVATION CANCELLED");
      }
    }
    stateChanged = true;
    DynamicJsonDocument doc(128);
    doc["success"] = success;
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    localServer.sendHeader("Access-Control-Allow-Origin", "*");
    localServer.send(200, "application/json", jsonResponse);
  });

  localServer.on("/", HTTP_GET, []() {
    String html = "<html><head><title>Smart Parking</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:Arial;text-align:center;margin:20px}";
    html += ".slot{width:120px;height:160px;display:inline-block;margin:10px;border-radius:10px;color:white;padding:10px}";
    html += ".available{background-color:#4CAF50}.occupied{background-color:#e53935}.reserved{background-color:#ff9800}";
    html += "button{padding:8px 15px;border:none;border-radius:5px;margin-top:10px;cursor:pointer}";
    html += "</style></head><body>";
    html += "<h1>Smart Parking Dashboard</h1>";
    html += "<div id='parking-area'></div>";
    html += "<p>Local NodeMCU Server IP: " + WiFi.localIP().toString() + "</p>";
    html += "<script>";
    html += "function updateStatus(){";
    html += "fetch('/status').then(r=>r.json()).then(data=>{";
    html += "let html='';";
    html += "for(let i=1;i<=2;i++){";
    html += "let slot=data['slot'+i];";
    html += "let status='Available';let cls='available';";
    html += "if(slot.occupied){status='Occupied';cls='occupied';}";
    html += "else if(slot.reserved){status='Reserved';cls='reserved';}";
    html += "html+='<div class=\"slot '+cls+'\"><h3>Slot '+i+'</h3>';";
    html += "html+='<p>'+status+'</p>';";
    html += "if(!slot.occupied){";
    html += "if(slot.reserved){";
    html += "html+='<button onclick=\"reserve('+i+',0)\">Cancel Reservation</button>';}";
    html += "else{html+='<button onclick=\"reserve('+i+',1)\">Reserve Slot</button>';}}";
    html += "html+='</div>';}";
    html += "document.getElementById('parking-area').innerHTML=html;}).catch(e=>console.error('Error:',e));}";
    html += "function reserve(slot,status){fetch(`/reserve?slot=${slot}&status=${status}`).then(r=>r.json()).then(data=>{if(data.success)updateStatus();}).catch(e=>console.error('Error:',e));}";
    html += "updateStatus();setInterval(updateStatus,1000);";
    html += "</script></body></html>";
    localServer.send(200, "text/html", html);
  });
  localServer.begin();
  Serial.println("Local web server started");
}

long readDistanceCM(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH, 30000);
  if (duration == 0) return -1;
  return duration * 0.034 / 2;
}

long getStableDistance(long distances[]) {
  long tempDist[READING_SAMPLES];
  int validReadings = 0;
  for (int i = 0; i < READING_SAMPLES; i++) {
    if (distances[i] != -1) {
      tempDist[validReadings] = distances[i];
      validReadings++;
    }
  }
  if (validReadings == 0) return -1;
  for (int i = 0; i < validReadings - 1; i++) {
    for (int j = i + 1; j < validReadings; j++) {
      if (tempDist[i] > tempDist[j]) {
        long temp = tempDist[i];
        tempDist[i] = tempDist[j];
        tempDist[j] = temp;
      }
    }
  }
  return tempDist[validReadings / 2];
}

void checkReservations() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = "http://api.thingspeak.com/channels/";
  url += channelID;
  url += "/fields/3,4/last.json?api_key=";
  url += readApiKey;
  http.begin(client, url);
  int httpCode = http.GET();
  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, payload);
      if (doc.containsKey("field3")) {
        int field3 = doc["field3"].as<int>();
        slot1Reserved = (field3 == 1);
      }
      if (doc.containsKey("field4")) {
        int field4 = doc["field4"].as<int>();
        slot2Reserved = (field4 == 1);
      }
      digitalWrite(LED_RESERVE_1, slot1Reserved ? HIGH : LOW);
      digitalWrite(LED_RESERVE_2, slot2Reserved ? HIGH : LOW);
    }
  }
  http.end();
}

void updateThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) return;
  int slot1 = slot1Occupied ? 1 : 0;
  int slot2 = slot2Occupied ? 1 : 0;
  int reserve1 = slot1Reserved ? 1 : 0;
  int reserve2 = slot2Reserved ? 1 : 0;
  ThingSpeak.setField(1, slot1);
  ThingSpeak.setField(2, slot2);
  ThingSpeak.setField(3, reserve1);
  ThingSpeak.setField(4, reserve2);
  ThingSpeak.writeFields(channelID, writeApiKey);
}

void controlEntryGate() {
  if (digitalRead(IR_SENSOR) == LOW) {
    if (!gateIsOpen) {
      entryGate.write(GATE_OPEN_POS);
      gateIsOpen = true;
      gateOpenTime = millis();
    }
  }
  if (gateIsOpen && (millis() - gateOpenTime > gateOpenDuration)) {
    entryGate.write(GATE_CLOSED_POS);
    gateIsOpen = false;
  }
}

void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(ssid, wifiPassword);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
      delay(500);
      attempts++;
    }
  }
}

void loop() {
  unsigned long currentMillis = millis();
  localServer.handleClient();
  controlEntryGate();
  if (currentMillis - lastSensorRead > sensorReadInterval) {
    slot1Distances[sampleIndex] = readDistanceCM(TRIG_PIN_1, ECHO_PIN_1);
    slot2Distances[sampleIndex] = readDistanceCM(TRIG_PIN_2, ECHO_PIN_2);
    sampleIndex = (sampleIndex + 1) % READING_SAMPLES;
    long stableDistance1 = getStableDistance(slot1Distances);
    long stableDistance2 = getStableDistance(slot2Distances);
    slot1PrevOccupied = slot1Occupied;
    slot2PrevOccupied = slot2Occupied;
    slot1PrevReserved = slot1Reserved;
    slot2PrevReserved = slot2Reserved;
    if (stableDistance1 != -1) {
      slot1Occupied = (stableDistance1 < 10);
      digitalWrite(LED_OCCUPIED_1, slot1Occupied ? HIGH : LOW);
    }
    if (stableDistance2 != -1) {
      slot2Occupied = (stableDistance2 < 10);
      digitalWrite(LED_OCCUPIED_2, slot2Occupied ? HIGH : LOW);
    }
    if (slot1Occupied != slot1PrevOccupied || slot2Occupied != slot2PrevOccupied ||
        slot1Reserved != slot1PrevReserved || slot2Reserved != slot2PrevReserved) {
      stateChanged = true;
    }
    lastSensorRead = currentMillis;
  }
  if ((currentMillis - lastThingSpeakUpdate > thingSpeakInterval) || stateChanged) {
    updateThingSpeak();
    stateChanged = false;
    lastThingSpeakUpdate = currentMillis;
  }
  if (currentMillis % 30000 == 0) {
    checkWiFiConnection();
  }
  yield();
}
