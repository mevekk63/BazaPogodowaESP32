#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>

#define LED_PIN    38
#define LED_COUNT  1

Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_RGB + NEO_KHZ800);
WebServer server(80);
Preferences preferences;

// ==========================================
// ZMIENNE GLOBALNE
// ==========================================
String ssid = "";
String password = "";
String telegramToken = "";
String chatId = "";
String lat = "49.8450"; // Domyślnie Hałcnów
String lon = "19.0880";

// Limity obserwacyjne
const int MAX_CLOUDS      = 10;
const int MAX_WIND        = 15;
const int NIGHT_START     = 21;
const int NIGHT_END       = 4;

const String apiBase = "https://api.open-meteo.com/v1/forecast?";
bool apMode = false;

struct AstroData {
  String statusMsg;
  String windowMsg;
  int currentClouds;
  int currentHumidity;
  bool isWindowFound;
  bool notificationSent;
  unsigned long lastUpdate;
};

AstroData astro = {"Czekam na pierwsze dane...", "Brak danych.", 0, 0, false, false, 0};

// ==========================================
// FUNKCJE POMOCNICZE
// ==========================================

String urlencode(String str) {
  String encodedString = "";
  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c == ' ') encodedString += '+';
    else if (isalnum(c)) encodedString += c;
    else {
      char code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
      c = (c >> 4) & 0xf;
      char code0 = c + '0';
      if (c > 9) code0 = c - 10 + 'A';
      encodedString += '%';
      encodedString += code0;
      encodedString += code1;
    }
  }
  return encodedString;
}

void sendTelegram(String msg) {
  if (WiFi.status() != WL_CONNECTED || telegramToken == "" || chatId == "") return;
  
  HTTPClient http;
  String fullUrl = "https://api.telegram.org/bot" + telegramToken + "/sendMessage?chat_id=" + chatId + "&text=" + urlencode(msg);
  http.begin(fullUrl);
  int httpCode = http.GET();
  
  if(httpCode == 200) {
    Serial.println("[TELEGRAM] Pomyślnie wysłano powiadomienie.");
  } else {
    Serial.println("[TELEGRAM] Błąd wysyłania. Kod: " + String(httpCode));
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
  Serial.println("[MEM] Ustawienia załadowane z pamięci Flash.");
}

// ==========================================
// LOGIKA API
// ==========================================

void fetchAndAnalyze() {
  if (WiFi.status() != WL_CONNECTED || apMode) return;

  if (lat == "" || lon == "") {
      Serial.println("[API] Błąd: Brak współrzędnych!");
      return;
  }

  String fullUrl = apiBase + "latitude=" + lat + "&longitude=" + lon + 
                   "&hourly=cloudcover,windspeed_10m,windspeed_80m,relativehumidity_2m&forecast_hours=24&timezone=Europe%2FWarsaw";

  Serial.println("[API] Pobieram dane dla: " + lat + ", " + lon);
  HTTPClient http;
  http.begin(fullUrl);
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(32768); 
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
      Serial.println("[JSON] Błąd parsowania: " + String(error.c_str())); 
      return;
    }

    JsonArray clouds = doc["hourly"]["cloudcover"];
    JsonArray wind10 = doc["hourly"]["windspeed_10m"];
    JsonArray wind80 = doc["hourly"]["windspeed_80m"];
    JsonArray humidity = doc["hourly"]["relativehumidity_2m"];
    JsonArray times  = doc["hourly"]["time"];
    
    astro.currentClouds = clouds[0];
    astro.currentHumidity = humidity[0];
    astro.statusMsg = "Chmury: " + String(astro.currentClouds) + "%. Wilgotność: " + String(astro.currentHumidity) + "%. Wiatr: " + String((int)wind10[0]) + " km/h";

    String windowStart = "";
    String windowEnd = "";
    float bestSeeingScore = 0;
    String bestHourStr = "";
    bool inWindow = false;

    for (int i = 0; i < 24; i++) {
      String timeString = String((const char*)times[i]);
      int hour = timeString.substring(11, 13).toInt();

      bool isNight = (hour >= NIGHT_START || hour <= NIGHT_END);
      if (!isNight) {
        if (inWindow) break; 
        continue;
      }

      float windShear = abs((float)wind10[i] - (float)wind80[i]);
      float score = 100 - (float)clouds[i] - (windShear * 1.5); 
      
      if (clouds[i] <= MAX_CLOUDS && wind10[i] <= MAX_WIND) { 
        if (!inWindow) {
          windowStart = timeString.substring(11, 16);
          inWindow = true;
        }
        windowEnd = timeString.substring(11, 16);
        
        if (score > bestSeeingScore) {
          bestSeeingScore = score;
          bestHourStr = timeString.substring(11, 16);
        }
      } else {
        if (inWindow) break;
      }
    }

    if (inWindow) {
      astro.isWindowFound = true;
      astro.windowMsg = "Okienko nocne: " + windowStart + " - " + windowEnd + "<br>Jakość szczytowa o: " + bestHourStr;
      
      if (!astro.notificationSent) {
        String msg = "🔭 ASTRO BAZA:\nSzykuj Teleskop!\nOkienko: " + windowStart + " do " + windowEnd + "\nNajlepszy seeing: " + bestHourStr;
        sendTelegram(msg);
        astro.notificationSent = true;
      }
    } else {
      astro.isWindowFound = false;
      astro.windowMsg = "Brak odpowiednich warunków w nocy.";
      astro.notificationSent = false;
    }
    
    astro.lastUpdate = millis();
    Serial.println("[API] Zaktualizowano. Następne okno: " + windowStart);

  } else {
    Serial.println("[API] Błąd: HTTP " + String(httpCode));
  }
  http.end();
}

// ==========================================
// WEBSERVER I HTML
// ==========================================

String getHeader(String title, bool autoRefresh) {
  String html = "<!DOCTYPE html><html lang='pl'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  if(autoRefresh && !apMode) html += "<meta http-equiv='refresh' content='60'>";
  html += "<title>" + title + "</title>";
  html += "<style>";
  html += "body { background-color: #0f172a; color: #f8fafc; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; text-align: center; margin: 0; padding: 20px; }";
  html += "h1 { color: #38bdf8; letter-spacing: 2px; }";
  html += ".card { background-color: #1e293b; padding: 25px; border-radius: 12px; display: inline-block; box-shadow: 0 10px 15px -3px rgba(0,0,0,0.5); width: 90%; max-width: 400px; margin-top: 20px;}";
  html += ".status-ok { border-left: 5px solid #22c55e; }";
  html += ".status-bad { border-left: 5px solid #ef4444; }";
  html += ".status-ap { border-left: 5px solid #f59e0b; }";
  html += "h2 { color: #94a3b8; font-size: 1.2rem; margin-bottom: 5px; }";
  html += "p { font-size: 1.1rem; margin-top: 5px; line-spacing: 1.4; }";
  html += "button { background-color: #0284c7; color: white; border: none; padding: 12px 25px; border-radius: 6px; cursor: pointer; font-size: 16px; margin-top: 15px; font-weight: bold; transition: background 0.3s; width: 100%; box-sizing: border-box;}";
  html += "button:hover { background-color: #0369a1; }";
  html += ".btn-sec { background-color: #475569; margin-top: 10px;}";
  html += ".btn-sec:hover { background-color: #334155; }";
  html += "input { width: 100%; padding: 10px; margin: 8px 0 20px 0; border-radius: 6px; border: 1px solid #334155; background: #0f172a; color: white; box-sizing: border-box; }";
  html += "label { display: block; text-align: left; color: #94a3b8; font-size: 0.9rem;}";
  html += ".footer { margin-top: 20px; font-size: 0.8rem; color: #475569; }";
  html += "</style></head><body>";
  return html;
}

void handleRoot() {
  if (apMode) {
    server.sendHeader("Location", "/settings", true);
    server.send(302, "text/plain", "");
    return;
  }

  String html = getHeader("Astro Baza Panel", true);
  html += "<h1>BAZA ASTRO</h1>";
  
  String cardClass = astro.isWindowFound ? "card status-ok" : "card status-bad";
  
  html += "<div class='" + cardClass + "'>";
  html += "<h2>TERAZ</h2><p>" + astro.statusMsg + "</p><hr style='border:1px solid #334155; margin: 15px 0;'>";
  html += "<h2>NAJBLIŻSZE OKNO</h2><p>" + astro.windowMsg + "</p>";
  html += "</div><br>";
  
  html += "<div style='max-width: 400px; margin: 0 auto;'>";
  html += "<button onclick='location.reload()'>ODŚWIEŻ</button>";
  html += "<button class='btn-sec' onclick='location.href=\"/settings\"'>⚙️ USTAWIENIA</button>";
  html += "</div>";
  
  unsigned long secondsAgo = (millis() - astro.lastUpdate) / 1000;
  html += "<div class='footer'>Ostatni odczyt z serwera: " + String(secondsAgo) + " s temu.</div>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleSettings() {
  String html = getHeader("Ustawienia - Astro Baza", false);
  html += "<h1>⚙️ KONFIGURACJA</h1>";
  
  if(apMode) {
    html += "<p style='color: #f59e0b;'>Jesteś w trybie Offline.<br>Wprowadź dane sieci WiFi, aby połączyć się z internetem.</p>";
  }

  html += "<div class='card status-ap' style='text-align: left;'>";
  html += "<form action='/save' method='POST'>";

  html += "<label>Nazwa WiFi (SSID 2.4GHz):</label>";
  html += "<input type='text' name='ssid' value='" + ssid + "' required placeholder='np. Orange_24'>";
  
  html += "<label>Hasło WiFi:</label>";
  html += "<input type='password' name='password' value='" + password + "'>";
  
  html += "<label>Telegram Bot Token:</label>";
  html += "<input type='text' name='telegramToken' value='" + telegramToken + "' placeholder='12345:ABCDE...'>";
  
  html += "<label>Telegram Chat ID:</label>";
  html += "<input type='text' name='chatId' value='" + chatId + "'>";
  
  html += "<label>Szerokość geogr. (LAT):</label>";
  html += "<input type='text' name='lat' value='" + lat + "' required pattern='-?\\d+(\\.\\d+)?' title='Podaj liczbę z kropką, np. 49.8450'>";
  
  html += "<label>Długość geogr. (LON):</label>";
  html += "<input type='text' name='lon' value='" + lon + "' required pattern='-?\\d+(\\.\\d+)?' title='Podaj liczbę z kropką, np. 19.0880'>";
  
  html += "<button type='submit'>💾 ZAPISZ I RESTARTUJ</button>";
  html += "</form>";
  
  if(!apMode) {
    html += "<button class='btn-sec' onclick='location.href=\"/\"'>Wróć do bazy</button>";
  }
  
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.method() == HTTP_POST) {
    // Zapisujemy nowe dane - tym razem otwieramy w trybie do zapisu
    preferences.begin("astro", false);
    preferences.putString("ssid", server.arg("ssid"));
    preferences.putString("password", server.arg("password"));
    preferences.putString("telegramToken", server.arg("telegramToken"));
    preferences.putString("chatId", server.arg("chatId"));
    preferences.putString("lat", server.arg("lat"));
    preferences.putString("lon", server.arg("lon"));
    preferences.end(); // Zamykamy pamięć po zapisie!

    String html = getHeader("Zapisywanie...", false);
    html += "<h1>ZAPISANO! ✅</h1><p>Sieć konfiguracyjna zostaje wyłączona.</p><p>Trwa restartowanie urządzenia... Połącz się ze swoim domowym WiFi i wejdź pod nowy adres IP.</p></body></html>";
    server.send(200, "text/html", html);
    
    delay(1000); 
    WiFi.softAPdisconnect(true);
    delay(1000);
    ESP.restart();
  } else {
    server.send(405, "text/plain", "Method Not Allowed");
  }
}

// ==========================================
// SETUP & LOOP
// ==========================================

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
    WiFi.softAPdisconnect(true); // Zabezpieczenie: gwarancja ubicia niewidzialnego "zombie AP" po restarcie
    WiFi.mode(WIFI_STA); // Wymuszenie tylko pracy jako klient
    apMode = false;
  } else {
    Serial.println("\n[WIFI] Błąd połączenia lub brak danych! Uruchamiam tryb Konfiguracji (AP).");
    WiFi.disconnect(true, true); // Zrzucamy stację zanim włączymy AP
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
  
  if(!apMode) {
    fetchAndAnalyze();
  }
}

void loop() {
  server.handleClient(); 
  
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
