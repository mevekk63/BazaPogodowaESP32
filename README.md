# BazaPogodowaESP32
Prosta baza pogodowa która mówi nam przez webserver i wysyła powiadomienie na telegram o stanie i jakości pogodowej do obserwacji astronomicznych

## 1. Uzyskanie Tokenu Bota (telegramToken)
Token pozwala Twojemu ESP32 sterować botem.

1. Znajdź w wyszukiwarce Telegrama użytkownika **@BotFather**.
2. Rozpocznij czat i wpisz komendę: `/newbot`.
3. Podaj nazwę wyświetlaną bota (np. `Astro Bot`).
4. Podaj unikalną nazwę użytkownika kończącą się na `bot` (np. `GigaAstro_bot`).
5. Po zatwierdzeniu otrzymasz **HTTP API Token**. Skopiuj go do kodu:
   ```cpp
   const char* telegramToken = "TWÓJ_TOKEN_Z_BOTFATHER";

## 2. Uzyskanie ID użytkownika (chatId)
ID jest niezbędne, aby bot wiedział, do kogo konkretnie ma wysyłać wiadomości (zabezpieczenie prywatności).

1. Znajdź w wyszukiwarce Telegrama użytkownika **@userinfobot**.
2. Kliknij Start lub wyślij dowolną wiadomość.
3. Bot odeśle Ci Twój unikalny numer (np. 6864245306).
4. Skopiuj ten numer do kodu:
   ```cpp
   const char* chatId = "TWÓJ_NUMER_ID";

Resztę czyli ssid i password to nazwa sieci wifi(2.4Ghz) oraz hasło
