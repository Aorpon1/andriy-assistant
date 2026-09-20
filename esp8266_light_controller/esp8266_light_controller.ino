/*
 * ============================================================================
 *  ESP8266 — WiFi світло-контролер з веб-мордою
 * ============================================================================
 *
 *  ЩО РОБИТЬ ЦЯ ПРОШИВКА:
 *   - ESP8266 сама роздає WiFi (режим Access Point / точка доступу).
 *   - Ти підключаєшся телефоном до цієї WiFi і заходиш у браузер.
 *   - На сторінці керуєш світлом (LED на піні D7):
 *       * кнопка ON / OFF (увімк / вимк)
 *       * повзунок яскравості (0..100%)
 *       * режим СТРОБ (блимання) + повзунок швидкості строба
 *   - LED вмикається ТІЛЬКИ з веб-морди (за замовчуванням вимкнений).
 *
 *  ПЛАТА:  ESP8266 (NodeMCU / Wemos D1 mini) — це НЕ ESP32!
 *          PWM на ESP8266 має діапазон 0..1023 (а не 0..255 як на Arduino/ESP32).
 *
 *  ПІН СВІТЛА:  D7 = GPIO13 — вільний, підтримує PWM, безпечний при завантаженні.
 *
 *  УВАГА по живленню світла:
 *   - Пін ESP8266 дає ~3.3В і максимум ~12мА. Це вистачить лише для маленького
 *     індикаторного світлодіода (з резистором 220..330 Ом!).
 *   - Для потужного світла (стрічка / потужний LED) пін керує НЕ самим світлом,
 *     а транзистором/MOSFET-модулем (наприклад IRLZ44N або готовий MOSFET-модуль),
 *     а вже MOSFET комутує окреме живлення світла. Логіка коду однакова.
 * ============================================================================
 */

#include <ESP8266WiFi.h>          // WiFi для ESP8266
#include <ESP8266WebServer.h>     // простий веб-сервер

// ----------------------------------------------------------------------------
//  1) НАЛАШТУВАННЯ WiFi ТОЧКИ ДОСТУПУ (те, що роздає плата)
// ----------------------------------------------------------------------------
const char* AP_SSID = "FPV-Light";     // назва WiFi мережі (можеш змінити)
const char* AP_PASS = "12345678";      // пароль (МІНІМУМ 8 символів!)
// Якщо хочеш WiFi без пароля — постав: const char* AP_PASS = "";

// ----------------------------------------------------------------------------
//  2) ПІН СВІТЛА
// ----------------------------------------------------------------------------
const int LED_PIN = D7;                 // GPIO13 — світло / MOSFET-затвор

// Якщо світло горить, коли має бути вимкнене (інвертована схема / деякі MOSFET),
// постав true — тоді сигнал інвертується:
const bool LED_INVERT = false;

// ----------------------------------------------------------------------------
//  3) ГЛОБАЛЬНИЙ СТАН СВІТЛА (початкові значення = "дефолтне включення/виключення")
// ----------------------------------------------------------------------------
bool ledOn        = false;   // false = за замовчуванням світло ВИМКНЕНЕ
int  brightness   = 60;      // яскравість у відсотках 0..100 (стартове значення)
bool strobeOn     = false;   // режим строба вимкнений за замовчуванням
int  strobeHz     = 8;       // швидкість строба у "спалахах за секунду" (1..20)

// Внутрішні змінні для строба (не чіпати):
unsigned long lastStrobeToggle = 0;   // коли востаннє перемикали спалах
bool          strobePhaseOn    = false; // поточна фаза строба (горить/не горить)

// Веб-сервер на 80-му порту (стандартний http порт)
ESP8266WebServer server(80);

// ============================================================================
//  ДОПОМІЖНА ФУНКЦІЯ: застосувати поточний стан до фізичного піна
// ============================================================================
// Перетворює відсотки (0..100) у значення PWM ESP8266 (0..1023)
int brightnessToPWM(int percent) {
  if (percent < 0)   percent = 0;
  if (percent > 100) percent = 100;
  // map() лінійно масштабує діапазон: 0..100  ->  0..1023
  return map(percent, 0, 100, 0, 1023);
}

// Записує потрібне значення PWM у пін з урахуванням інверсії
void writePWM(int pwm) {
  if (LED_INVERT) pwm = 1023 - pwm;   // інвертуємо, якщо схема того вимагає
  analogWrite(LED_PIN, pwm);
}

// Головна функція керування світлом — викликається в loop()
void applyLight() {
  // 1) Якщо світло вимкнене загальною кнопкою — гасимо і виходимо
  if (!ledOn) {
    writePWM(0);
    strobePhaseOn = false;
    return;
  }

  // 2) Якщо строб вимкнений — просто світимо з потрібною яскравістю
  if (!strobeOn) {
    writePWM(brightnessToPWM(brightness));
    return;
  }

  // 3) Режим СТРОБ: блимаємо з частотою strobeHz.
  //    Період одного повного циклу (вкл+викл) = 1000 / strobeHz мілісекунд.
  //    Половина періоду горить, половина — темрява.
  unsigned long halfPeriod = 500UL / (unsigned long)strobeHz; // мс на одну фазу
  unsigned long now = millis();
  if (now - lastStrobeToggle >= halfPeriod) {
    lastStrobeToggle = now;
    strobePhaseOn = !strobePhaseOn;      // перемикаємо фазу
  }
  // У фазі "горить" — світимо на заданій яскравості, інакше — темрява
  writePWM(strobePhaseOn ? brightnessToPWM(brightness) : 0);
}

// ============================================================================
//  HTML-СТОРІНКА (веб-морда). Зберігається в пам'яті програми (F-макрос),
//  щоб не з'їдати оперативку. JavaScript всередині шле команди на плату.
// ============================================================================
String buildPage() {
  String html = F(
    "<!DOCTYPE html><html lang='uk'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
    "<title>FPV Light</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,Segoe UI,sans-serif}"
    "body{background:#0d0a1a;color:#f0e9ff;min-height:100vh;padding:24px 16px;"
    "background-image:radial-gradient(ellipse at 30% 0%,rgba(109,40,217,.25),transparent 55%)}"
    ".card{max-width:420px;margin:0 auto;background:rgba(255,255,255,.04);"
    "border:1px solid rgba(167,139,250,.15);border-radius:20px;padding:24px}"
    "h1{font-size:22px;margin-bottom:4px}"
    ".sub{color:#a78bfa;font-size:13px;margin-bottom:22px}"
    ".row{margin-bottom:22px}"
    ".row label{display:block;font-size:14px;margin-bottom:10px;color:#cbb9ff}"
    ".val{float:right;color:#fff;font-weight:600}"
    "button{width:100%;padding:16px;border:none;border-radius:14px;font-size:17px;"
    "font-weight:700;cursor:pointer;transition:.15s}"
    ".btn-on{background:#7c3aed;color:#fff}"
    ".btn-off{background:rgba(255,255,255,.08);color:#f0e9ff}"
    ".btn-strobe-on{background:#f59e0b;color:#1a1206}"
    ".btn-strobe-off{background:rgba(255,255,255,.08);color:#f0e9ff}"
    "input[type=range]{width:100%;height:8px;border-radius:8px;appearance:none;"
    "background:rgba(167,139,250,.25);outline:none}"
    "input[type=range]::-webkit-slider-thumb{appearance:none;width:26px;height:26px;"
    "border-radius:50%;background:#a78bfa;cursor:pointer}"
    ".hint{font-size:12px;color:#7c6f99;text-align:center;margin-top:8px}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>FPV Light</h1>"
    "<div class='sub'>Керування світлом ESP8266</div>"

    "<div class='row'>"
    "<button id='power' class='btn-off' onclick='togglePower()'>УВІМКНУТИ</button>"
    "</div>"

    "<div class='row'>"
    "<label>Яскравість <span class='val'><span id='bval'>60</span>%</span></label>"
    "<input type='range' id='bright' min='0' max='100' value='60' "
    "oninput='onBright(this.value)'>"
    "</div>"

    "<div class='row'>"
    "<button id='strobe' class='btn-strobe-off' onclick='toggleStrobe()'>СТРОБ: ВИМК</button>"
    "</div>"

    "<div class='row'>"
    "<label>Швидкість строба <span class='val'><span id='sval'>8</span> Гц</span></label>"
    "<input type='range' id='speed' min='1' max='20' value='8' "
    "oninput='onSpeed(this.value)'>"
    "</div>"

    "<div class='hint'>Стан зберігається на платі</div>"
    "</div>"

    // ---------- JAVASCRIPT ----------
    "<script>"
    "let on=false, strobe=false;"
    // Універсальна відправка команди на плату: /set?param=value
    "function send(q){fetch('/set?'+q).then(r=>r.json()).then(apply);}"
    // Оновлює вигляд сторінки під отриманий від плати стан
    "function apply(s){"
    "  on=s.on; strobe=s.strobe;"
    "  let p=document.getElementById('power');"
    "  p.textContent=on?'ВИМКНУТИ':'УВІМКНУТИ';"
    "  p.className=on?'btn-on':'btn-off';"
    "  let st=document.getElementById('strobe');"
    "  st.textContent=strobe?'СТРОБ: УВІМК':'СТРОБ: ВИМК';"
    "  st.className=strobe?'btn-strobe-on':'btn-strobe-off';"
    "  document.getElementById('bright').value=s.bright;"
    "  document.getElementById('bval').textContent=s.bright;"
    "  document.getElementById('speed').value=s.hz;"
    "  document.getElementById('sval').textContent=s.hz;"
    "}"
    "function togglePower(){send('on='+(on?0:1));}"
    "function toggleStrobe(){send('strobe='+(strobe?0:1));}"
    "function onBright(v){document.getElementById('bval').textContent=v;send('bright='+v);}"
    "function onSpeed(v){document.getElementById('sval').textContent=v;send('hz='+v);}"
    // При завантаженні сторінки — питаємо плату про поточний стан
    "fetch('/state').then(r=>r.json()).then(apply);"
    "</script>"
    "</body></html>"
  );
  return html;
}

// ============================================================================
//  ОБРОБНИКИ HTTP-ЗАПИТІВ
// ============================================================================

// Формує JSON з поточним станом — його читає JavaScript
String stateJson() {
  String j = "{";
  j += "\"on\":"      + String(ledOn ? 1 : 0) + ",";
  j += "\"bright\":"  + String(brightness) + ",";
  j += "\"strobe\":"  + String(strobeOn ? 1 : 0) + ",";
  j += "\"hz\":"      + String(strobeHz);
  j += "}";
  return j;
}

// GET "/"  -> віддати HTML сторінку
void handleRoot() {
  server.send(200, "text/html", buildPage());
}

// GET "/state" -> віддати поточний стан у JSON
void handleState() {
  server.send(200, "application/json", stateJson());
}

// GET "/set?on=1&bright=50&strobe=0&hz=10" -> змінити стан і повернути новий JSON
void handleSet() {
  // Кожен параметр перевіряємо: чи він взагалі переданий у запиті
  if (server.hasArg("on")) {
    ledOn = (server.arg("on").toInt() == 1);
  }
  if (server.hasArg("bright")) {
    int b = server.arg("bright").toInt();
    if (b < 0) b = 0;  if (b > 100) b = 100;   // захист від сміття
    brightness = b;
  }
  if (server.hasArg("strobe")) {
    strobeOn = (server.arg("strobe").toInt() == 1);
    lastStrobeToggle = millis();  // скидаємо таймер строба, щоб почалось чисто
    strobePhaseOn = false;
  }
  if (server.hasArg("hz")) {
    int h = server.arg("hz").toInt();
    if (h < 1)  h = 1;   if (h > 20) h = 20;    // безпечні межі частоти
    strobeHz = h;
  }
  applyLight();                                  // одразу застосувати
  server.send(200, "application/json", stateJson());
}

// Будь-яка інша адреса -> 404
void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ============================================================================
//  SETUP — виконується один раз при старті плати
// ============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("=== ESP8266 Light Controller ==="));

  // Налаштовуємо пін світла як вихід і одразу гасимо (дефолт = вимкнено)
  pinMode(LED_PIN, OUTPUT);
  analogWriteRange(1023);     // явно задаємо діапазон PWM 0..1023 (стандарт ESP8266)
  analogWriteFreq(1000);      // частота PWM 1 кГц — без видимого мерехтіння
  writePWM(0);                // світло вимкнене

  // Піднімаємо WiFi точку доступу.
  // Якщо пароль коротший за 8 символів — робимо відкриту мережу (вимога WiFi).
  WiFi.mode(WIFI_AP);
  bool ok;
  if (strlen(AP_PASS) >= 8) {
    ok = WiFi.softAP(AP_SSID, AP_PASS);
  } else {
    ok = WiFi.softAP(AP_SSID);          // без пароля
  }

  Serial.print(F("Точка доступу: "));
  Serial.println(ok ? F("OK") : F("ПОМИЛКА"));
  Serial.print(F("SSID: "));  Serial.println(AP_SSID);
  Serial.print(F("IP адреса: "));
  Serial.println(WiFi.softAPIP());      // зазвичай 192.168.4.1

  // Реєструємо маршрути веб-сервера
  server.on("/",      handleRoot);
  server.on("/state", handleState);
  server.on("/set",   handleSet);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println(F("Веб-сервер запущено."));
  Serial.println(F("Відкрий у браузері: http://192.168.4.1"));
}

// ============================================================================
//  LOOP — виконується безкінечно
// ============================================================================
void loop() {
  server.handleClient();   // обробляємо запити з браузера
  applyLight();            // підтримуємо світло/строб у правильному стані
  yield();                 // віддаємо час системним задачам WiFi (важливо!)
}
