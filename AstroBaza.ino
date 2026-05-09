#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <WiFiClientSecure.h> 

#define LED_PIN    38
#define LED_COUNT  1

Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_RGB + NEO_KHZ800);
WebServer server(80);
Preferences preferences;

// Zmienne konfiguracyjne
String ssid = "";
String password = "";
String telegramToken = "";
String chatId = "";
String lat = "49.8450";
String lon = "19.0880";

// Parametry brzegowe
const int MAX_CLOUDS      = 10;
const int MAX_WIND        = 15;
const int NIGHT_START     = 21;
const int NIGHT_END       = 4;

const String apiBase = "https://api.open-meteo.com/v1/forecast?";
bool apMode = false; 

bool pendingReboot = false;
unsigned long rebootTime = 0;

// Stany okna pogodowego
enum WindowState {
  STATE_NONE,
  STATE_UPCOMING,
  STATE_ACTIVE
};

struct AstroData {
  String statusMsg;
  String windowMsg;
  int currentClouds;
  int currentHumidity;
  bool isWindowFound;
  WindowState winState; // Śledzenie stanu okienka
  unsigned long lastUpdate;
};

AstroData astro = {"Czekam na pierwsze dane...", "Brak danych.", 0, 0, false, STATE_NONE, 0};

const char CSS_STYLE[] PROGMEM = R"rawliteral(
<style>
  body { background-color: #0f172a; color: #f8fafc; font-family: 'Segoe UI', Tahoma, sans-serif; text-align: center; margin: 0; padding: 20px; }
  h1 { color: #38bdf8; letter-spacing: 2px; }
  .card { background-color: #1e293b; padding: 25px; border-radius: 12px; display: inline-block; box-shadow: 0 10px 15px -3px rgba(0,0,0,0.5); width: 90%; max-width: 400px; margin-top: 20px;}
  .status-ok { border-left: 5px solid #22c55e; }
  .status-bad { border-left: 5px solid #ef4444; }
  .status-ap { border-left: 5px solid #f59e0b; text-align: left; }
  h2 { color: #94a3b8; font-size: 1.2rem; margin-bottom: 5px; }
  p { font-size: 1.1rem; margin-top: 5px; line-height: 1.4; }
  button { background-color: #0284c7; color: white; border: none; padding: 12px 25px; border-radius: 6px; cursor: pointer; font-size: 16px; margin-top: 15px; font-weight: bold; transition: background 0.3s; width: 100%; box-sizing: border-box;}
  button:hover { background-color: #0369a1; }
  .btn-sec { background-color: #475569; margin-top: 10px;}
  .btn-sec:hover { background-color: #334155; }
  input { width: 100%; padding: 10px; margin: 8px 0 20px 0; border-radius: 6px; border: 1px solid #334155; background: #0f172a; color: white; box-sizing: border-box; }
  label { display: block; text-align: left; color: #94a3b8; font-size: 0.9rem;}
  .footer { margin-top: 20px; font-size: 0.8rem; color: #475569; }
</style>
)rawliteral";

void sendTelegram(String msg) {
  if (WiFi.status() != WL_CONNECTED || telegramToken == "" || chatId == "") return;
  
  String cleanToken = telegramToken; cleanToken.trim();
  String cleanChatId = chatId; cleanChatId.trim();

  WiFiClientSecure client;
  client.setInsecure(); 

  HTTPClient http;
  String fullUrl = "https://api.telegram.org/bot" + cleanToken + "/sendMessage";
  http.begin(client, fullUrl); 
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> jsonDoc;
  jsonDoc["chat_id"] = cleanChatId;
  jsonDoc["text"] = msg;
  String payload;
  serializeJson(jsonDoc, payload);

  int httpCode = http.POST(payload);
  
  if(httpCode == 200) {
    Serial.println("[TELEGRAM] Pomyślnie wysłano powiadomienie!");
  } else {
    Serial.println("[TELEGRAM] BŁĄD KRYTYCZNY. Kod HTTP: " + String(httpCode));
    String response = http.getString();
    Serial.println("[TELEGRAM] Odpowiedź serwera: " + response);
  }
  http.end();
}

void loadSettings() {
  preferences.begin("astro", true); 
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  telegramToken = preferences.getString("telegramToken", "");
  chatId = preferences.getString("chatId", "");
  
  String tempLat = preferences.getString("lat", "49.8450");
  String tempLon = preferences.getString("lon", "19.0880");
  if (tempLat.length() > 0) lat = tempLat;
  if (tempLon.length() > 0) lon = tempLon;
  
  preferences.end();
}

void fetchAndAnalyze() {
  if (WiFi.status() != WL_CONNECTED || apMode) return;

  if (lat == "" || lon == "") return;

  String fullUrl = apiBase + "latitude=" + lat + "&longitude=" + lon + 
                   "&hourly=cloudcover,windspeed_10m,windspeed_80m,relativehumidity_2m&forecast_hours=24&timezone=Europe%2FWarsaw";

  Serial.println("[API] Pobieram dane dla: " + lat + ", " + lon);
  
  WiFiClientSecure client;
  client.setInsecure(); 
  
  HTTPClient http;
  http.begin(client, fullUrl); 
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(32768); 
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) return;

    JsonArray clouds = doc["hourly"]["cloudcover"];
    JsonArray wind10 = doc["hourly"]["windspeed_10m"];
    JsonArray wind80 = doc["hourly"]["windspeed_80m"];
    JsonArray humidity = doc["hourly"]["relativehumidity_2m"];
    JsonArray times  = doc["hourly"]["time"];
    
    astro.currentClouds = clouds[0];
    astro.currentHumidity = humidity[0];
    astro.statusMsg = "Chmury: " + String(astro.currentClouds) + "%. Wilgotność: " + String(astro.currentHumidity) + "%. Wiatr: " + String((int)wind10[0]) + " km/h";

    int firstValidIdx = -1;
    int lastValidIdx = -1;
    float bestScore = 0;
    String bestHourStr = "";

    // Skanowanie indeksów (0 to obecna godzina)
    for (int i = 0; i < 24; i++) {
      String timeString = String((const char*)times[i]);
      int hour = timeString.substring(11, 13).toInt();

      bool isNight = (hour >= NIGHT_START || hour <= NIGHT_END);
      
      if (isNight) {
        float windShear = abs((float)wind10[i] - (float)wind80[i]);
        float score = 100 - (float)clouds[i] - (windShear * 1.5); 
        
        if (clouds[i] <= MAX_CLOUDS && wind10[i] <= MAX_WIND) { 
          if (firstValidIdx == -1) firstValidIdx = i;
          lastValidIdx = i;
          
          if (score > bestScore) {
            bestScore = score;
            bestHourStr = timeString.substring(11, 16);
          }
        } else {
          // Koniec ciągłego okna, przerywamy skanowanie
          if (firstValidIdx != -1) break;
        }
      } else {
        if (firstValidIdx != -1) break;
      }
    }

    // --- MASZYNA STANÓW (Powiadomienia) ---
    if (firstValidIdx != -1) {
      String startStr = String((const char*)times[firstValidIdx]).substring(11, 16);
      String endStr = String((const char*)times[lastValidIdx]).substring(11, 16);
      
      astro.isWindowFound = true;
      astro.windowMsg = "Okienko: " + startStr + " - " + endStr + "<br>Jakość szczytowa: " + bestHourStr;

      if (firstValidIdx > 0) {
        // Okno wystąpi w przyszłości
        if (astro.winState != STATE_UPCOMING) {
          sendTelegram("⏳ Zbliża się okno pogodowe!\nStart: " + startStr + "\nKoniec: " + endStr + "\nNajlepszy seeing: " + bestHourStr);
          astro.winState = STATE_UPCOMING;
        }
      } else { 
        // firstValidIdx == 0 -> Okno trwa WŁAŚNIE TERAZ
        if (astro.winState != STATE_ACTIVE) {
          sendTelegram("🟢 OKNO AKTYWNE!\nTeleskop na dwór!\nTrwa do: " + endStr + "\nSzczyt formy o: " + bestHourStr);
          astro.winState = STATE_ACTIVE;
        }
      }

    } else {
      // Brak okna (albo się skończyło, albo nie było)
      if (astro.winState == STATE_ACTIVE) {
        sendTelegram("🔴 Koniec okna pogodowego.\nWarunki się zepsuły albo wzeszło słońce. Pakuj sprzęt.");
      } else if (astro.winState == STATE_UPCOMING) {
        sendTelegram("❌ Okno odwołane.\nNiestety front pogodowy się zmienił i nici z obserwacji.");
      }
      
      astro.winState = STATE_NONE;
      astro.isWindowFound = false;
      astro.windowMsg = "Brak warunków w najbliższym czasie.";
    }
    
    astro.lastUpdate = millis();
    Serial.println("[API] Zaktualizowano dane pogodowe.");

  } else {
    Serial.println("[API] Błąd: HTTP " + String(httpCode));
  }
  http.end();
}

void handleRoot() {
  if (apMode) {
    server.sendHeader("Location", "/settings", true);
    server.send(302, "text/plain", "");
    return;
  }

  String html = R"rawliteral(
  <!DOCTYPE html>
  <html lang='pl'>
  <head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <meta http-equiv='refresh' content='60'>
    <title>Astro Baza Panel</title>
    {{STYLE}}
  </head>
  <body>
    <h1>BAZA ASTRO</h1>
    <div class='card {{CARD_CLASS}}'>
      <h2>TERAZ</h2><p>{{STATUS_MSG}}</p>
      <hr style='border:1px solid #334155; margin: 15px 0;'>
      <h2>NAJBLIŻSZE OKNO</h2><p>{{WINDOW_MSG}}</p>
    </div><br>
    <div style='max-width: 400px; margin: 0 auto;'>
      <button onclick='location.reload()'>ODŚWIEŻ</button>
      <button class='btn-sec' onclick='location.href="/settings"'>⚙️ USTAWIENIA</button>
    </div>
    <div class='footer'>Ostatni odczyt z serwera: {{TIME_AGO}} s temu.</div>
  </body>
  </html>
  )rawliteral";

  html.replace("{{STYLE}}", CSS_STYLE);
  html.replace("{{CARD_CLASS}}", astro.isWindowFound ? "status-ok" : "status-bad");
  html.replace("{{STATUS_MSG}}", astro.statusMsg);
  html.replace("{{WINDOW_MSG}}", astro.windowMsg);
  html.replace("{{TIME_AGO}}", String((millis() - astro.lastUpdate) / 1000));

  server.send(200, "text/html", html);
}

void handleSettings() {
  String html = R"rawliteral(
  <!DOCTYPE html>
  <html lang='pl'>
  <head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>Ustawienia - Astro Baza</title>
    {{STYLE}}
  </head>
  <body>
    <h1>⚙️ KONFIGURACJA</h1>
    {{AP_WARNING}}
    <div class='card status-ap'>
      <form action='/save' method='POST'>
        <label>Nazwa WiFi (SSID 2.4GHz):</label>
        <input type='text' name='ssid' value='{{SSID}}' required placeholder='np. Orange_24'>
        
        <label>Hasło WiFi:</label>
        <input type='password' name='password' value='{{PASSWORD}}'>
        
        <label>Telegram Bot Token:</label>
        <input type='text' name='telegramToken' value='{{TOKEN}}' placeholder='12345:ABCDE...'>
        
        <label>Telegram Chat ID:</label>
        <input type='text' name='chatId' value='{{CHAT_ID}}'>
        
        <label>Szerokość geogr. (LAT):</label>
        <input type='text' name='lat' value='{{LAT}}' required pattern='-?\d+(\.\d+)?' title='Liczba z kropką, np. 49.8450'>
        
        <label>Długość geogr. (LON):</label>
        <input type='text' name='lon' value='{{LON}}' required pattern='-?\d+(\.\d+)?' title='Liczba z kropką, np. 19.0880'>
        
        <button type='submit'>💾 ZAPISZ I RESTARTUJ</button>
      </form>
      {{BACK_BTN}}
    </div>
  </body>
  </html>
  )rawliteral";

  String apWarning = apMode ? "<p style='color: #f59e0b;'>Jesteś w trybie Offline.<br>Wprowadź dane sieci WiFi, aby połączyć się z internetem.</p>" : "";
  String backBtn = !apMode ? "<button class='btn-sec' onclick='location.href=\"/\"'>Wróć do bazy</button>" : "";

  html.replace("{{STYLE}}", CSS_STYLE);
  html.replace("{{AP_WARNING}}", apWarning);
  html.replace("{{SSID}}", ssid);
  html.replace("{{PASSWORD}}", password);
  html.replace("{{TOKEN}}", telegramToken);
  html.replace("{{CHAT_ID}}", chatId);
  html.replace("{{LAT}}", lat);
  html.replace("{{LON}}", lon);
  html.replace("{{BACK_BTN}}", backBtn);

  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.method() == HTTP_POST) {
    preferences.begin("astro", false);
    
    String s_ssid = server.arg("ssid"); s_ssid.trim();
    String s_pass = server.arg("password"); s_pass.trim();
    String s_token = server.arg("telegramToken"); s_token.trim();
    String s_chat = server.arg("chatId"); s_chat.trim();
    String s_lat = server.arg("lat"); s_lat.trim();
    String s_lon = server.arg("lon"); s_lon.trim();
    
    preferences.putString("ssid", s_ssid);
    preferences.putString("password", s_pass);
    preferences.putString("telegramToken", s_token);
    preferences.putString("chatId", s_chat);
    preferences.putString("lat", s_lat);
    preferences.putString("lon", s_lon);
    preferences.end(); 

    String html = R"rawliteral(
    <!DOCTYPE html><html lang='pl'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>Zapisywanie...</title>{{STYLE}}</head><body>
    <h1>ZAPISANO! ✅</h1>
    <p>Sieć konfiguracyjna zostaje wyłączona.</p>
    <p>Trwa restartowanie urządzenia... Połącz się ze swoim domowym WiFi i wejdź pod nowy adres IP.</p>
    </body></html>
    )rawliteral";
    html.replace("{{STYLE}}", CSS_STYLE);
    
    server.send(200, "text/html", html);
    
    pendingReboot = true;
    rebootTime = millis() + 2000;
  } else {
    server.send(405, "text/plain", "Method Not Allowed");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000); 

  led.begin();
  led.setBrightness(0);
  led.clear();
  led.show();

  setCpuFrequencyMhz(80); 
  
  loadSettings();

  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  if (ssid != "") {
    Serial.println("[WIFI] Próba łączenia z: " + ssid);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 30) {
      delay(500);
      Serial.print(".");
      retries++;
    }
  }

  if(WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] POŁĄCZONO! IP: " + WiFi.localIP().toString());
    WiFi.softAPdisconnect(true); 
    WiFi.mode(WIFI_STA); 
    apMode = false;
  } else {
    Serial.println("\n[WIFI] Błąd połączenia lub brak danych! Uruchamiam tryb Konfiguracji (AP).");
    WiFi.disconnect(true, true); 
    delay(100);
    
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Astro-Baza-Config");
    
    Serial.println("[AP] Połącz się z WiFi 'Astro-Baza-Config' i wejdź na http://192.168.4.1");
    apMode = true;
  }

  server.on("/", handleRoot);
  server.on("/settings", handleSettings);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

void loop() {
  server.handleClient(); 
  
  if (pendingReboot) {
    if (millis() > rebootTime) {
      WiFi.softAPdisconnect(true);
      delay(500);
      ESP.restart();
    }
    return; 
  }

  if (apMode) {
    delay(10);
    return;
  }
  
  static unsigned long lastApiCheck = 0;
  const unsigned long apiInterval = 900000; 
  
  static unsigned long lastWifiCheck = 0;
  const unsigned long wifiInterval = 30000;  

  if (millis() - lastWifiCheck >= wifiInterval) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Reanimacja połączenia...");
      WiFi.disconnect();
      WiFi.begin(ssid.c_str(), password.c_str());
      WiFi.setSleep(false);
    }
    lastWifiCheck = millis();
  }

  if (millis() - lastApiCheck >= apiInterval || lastApiCheck == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      fetchAndAnalyze();
      lastApiCheck = millis(); 
    }
  }
  delay(10);
}
