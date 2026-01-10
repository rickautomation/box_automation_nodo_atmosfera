#include <WiFi.h>              
#include <HTTPClient.h>        
#include <ArduinoJson.h>       
#include <Update.h>            
#include <WiFiClientSecure.h>  
#include <Preferences.h>        
#include <WebServer.h>          
#include <DNSServer.h>          
#include <DHT.h>

// ======================================================
// 0. VERSIÓN LOCAL DEL FIRMWARE
// ======================================================
const char* FIRMWARE_VERSION_CODE = "1.0.0";
String latestFirmwareVersion = FIRMWARE_VERSION_CODE;

// ======================================================
// 1. CONFIGURACIÓN DE RED Y FIREBASE
// ======================================================
const char* API_KEY = "AIzaSyAxGSXV2br1SsFu7YyP6NZaTXc_Z40uqA8"; 
const char* RTDB_HOST = "arduinoconfigremota-default-rtdb.firebaseio.com";                   

const char* DEFAULT_SSID = "tili";         
const char* DEFAULT_PASS = "Ubuntu1234$"; 

Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

const char* PREFS_NAMESPACE = "wifi_config";
const char* PREF_SSID = "ssid";
const char* PREF_PASS = "pass";
const char* AP_SSID = "NODO_ATMOSFERA_SETUP"; 

String loadedSsid = "";
String loadedPassword = "";
const int WIFI_RESET_PIN = 9; 

// ======================================================
// 2. CONFIGURACIÓN DINÁMICA
// ======================================================
String backendHost = "192.168.68.68";    
int backendPort = 3000;                  
String endpoint = "/sensor-data/atmosfera/batch"; 
long intervaloEnvioMs = 60000;            
bool flagActivo = true;                  
String remoteFirmwareVersion = "0.0.0"; 
String firmwareUrl = "";                 

const String RTDB_CONFIG_URL_BASE = "https://" + String(RTDB_HOST) + "/.json";
const char* NODE_TYPE_KEY = "NODO_ATMOSFERA"; 

// ======================================================
// 3. SENSORES Y DATOS
// ======================================================
String boxSerialId; 
const int DHT_PIN = 5;      
const int MQ_PIN = 4;       
#define DHTTYPE DHT22
DHT dht(DHT_PIN, DHTTYPE); 

float temp_val = 0;
float hum_val = 0;
int mq_raw_val = 0;

// ======================================================
// 4. FUNCIONES
// ======================================================
void leer_sensores();
bool conectar_wifi();
void enviar_post();
void startConfigPortal();
void obtener_remote_config();
bool check_for_update();

void setup() {
  Serial.begin(115200); 
  preferences.begin(PREFS_NAMESPACE, false);
  pinMode(WIFI_RESET_PIN, INPUT_PULLUP);
  dht.begin();

  WiFi.mode(WIFI_STA); 
  boxSerialId = WiFi.macAddress();
  boxSerialId.replace(":", ""); 
  
  Serial.println(F("\n--- ☁️ Nodo Atmósfera (v1.0.0) ☁️ ---"));

  if (digitalRead(WIFI_RESET_PIN) == LOW) {
    startConfigPortal(); 
  }

  loadedSsid = preferences.getString(PREF_SSID, DEFAULT_SSID);
  loadedPassword = preferences.getString(PREF_PASS, DEFAULT_PASS);

  if (conectar_wifi()) {
    obtener_remote_config();
    check_for_update();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

void loop() {
  unsigned long tiempoActual = millis();
  static unsigned long lastRun = 0;

  if (tiempoActual - lastRun >= intervaloEnvioMs) {
    lastRun = tiempoActual;

    // 1. Lectura
    leer_sensores();

    // 2. Comunicación
    if (conectar_wifi()) {
      if (flagActivo) enviar_post();
      obtener_remote_config();
      check_for_update();
      
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    }
    Serial.println(F("🔄 Ciclo completado. Esperando..."));
  }
}

void leer_sensores() {
  hum_val = dht.readHumidity();
  temp_val = dht.readTemperature();
  mq_raw_val = analogRead(MQ_PIN);

  if (isnan(hum_val) || isnan(temp_val)) {
    Serial.println(F("❌ Error DHT22"));
    hum_val = 0; temp_val = 0;
  }
}

void enviar_post() {
  DynamicJsonDocument doc(1024);
  doc["boxSerialId"] = boxSerialId;
  JsonArray dataArray = doc.createNestedArray("data");

  // Temperatura
  JsonObject t = dataArray.createNestedObject();
  t["arduinoPin"] = String(DHT_PIN);
  t["raw"] = (int)(temp_val * 100);
  t["unit"] = "C*100";
  t["key"] = "temperatura_ambiente";

  // Humedad
  JsonObject h = dataArray.createNestedObject();
  h["arduinoPin"] = String(DHT_PIN);
  h["raw"] = (int)(hum_val * 100);
  h["unit"] = "%RH*100";
  h["key"] = "humedad_ambiente";

  // MQ135
  JsonObject m = dataArray.createNestedObject();
  m["arduinoPin"] = String(MQ_PIN);
  m["raw"] = mq_raw_val;
  m["unit"] = "ADC";
  m["key"] = "calidad_aire_mq135";

  String jsonStr;
  serializeJson(doc, jsonStr);

  HTTPClient http;
  String url = "http://" + backendHost + ":" + String(backendPort) + endpoint;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  
  int code = http.POST(jsonStr);
  Serial.printf("📡 POST en %s -> Code: %d\n", endpoint.c_str(), code);
  http.end();
}

bool conectar_wifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(loadedSsid.c_str(), loadedPassword.c_str());
  unsigned long s = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - s < 15000) { delay(500); Serial.print("."); }
  return (WiFi.status() == WL_CONNECTED);
}

void obtener_remote_config() {
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(RTDB_CONFIG_URL_BASE + "?auth=" + API_KEY);
  if (http.GET() == 200) {
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, http.getString());
    backendHost = doc["remote_config"]["backend_host"].as<String>();
    backendPort = doc["remote_config"]["backend_port"].as<int>();
    // Usar endpoint específico para atmosfera
    if(doc["remote_config"]["endpoint_atmosfera"]) endpoint = doc["remote_config"]["endpoint_atmosfera"].as<String>();
    intervaloEnvioMs = doc["remote_config"]["intervalo_envio_ms"].as<long>();
    flagActivo = doc["remote_config"]["flag_activo"].as<bool>();
    
    JsonObject fw = doc["firmware_updates"][NODE_TYPE_KEY];
    remoteFirmwareVersion = fw["latest_firmware_version"].as<String>();
    firmwareUrl = fw["firmware_url"].as<String>();
    Serial.println(F("✅ Config actualizada."));
  }
  http.end();
}

bool check_for_update() {
  if (remoteFirmwareVersion == "0.0.0" || remoteFirmwareVersion == latestFirmwareVersion) return false;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (http.begin(client, firmwareUrl)) {
    if (http.GET() == 200 && Update.begin(http.getSize())) {
      Update.writeStream(http.getStream());
      if (Update.end()) ESP.restart();
    }
  } 
  return false;
}

void startConfigPortal() {
  WiFi.softAP(AP_SSID);
  dnsServer.start(53, "*", IPAddress(192,168,4,1));
  server.on("/", []() {
    server.send(200, "text/html", "<h1>Config Nodo Atmosfera</h1><form method='POST' action='/save'>SSID: <input name='s'><br>Pass: <input name='p' type='password'><br><input type='submit'></form>");
  });
  server.on("/save", []() {
    preferences.putString(PREF_SSID, server.arg("s"));
    preferences.putString(PREF_PASS, server.arg("p"));
    server.send(200, "text/html", "Guardado. Reiniciando...");
    delay(2000); ESP.restart();
  });
  server.begin();
  while(1) { dnsServer.processNextRequest(); server.handleClient(); }
}