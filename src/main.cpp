#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Preferences.h>

#define RST_PIN 4
#define NUM_LECTORES 4
#define MAX_MAESTRAS 10
#define MAX_EVENTOS_OFF 30

const char* NOMBRES[]     = {"norte", "sur",   "este",  "oeste"};
const uint8_t SS_PINS[]   = {5,       21,      27,      14};
const uint8_t LED_VERDE[] = {15,      25,      32,      16};
const uint8_t LED_ROJO[]  = {13,      26,      33,      17};

// Tarjeta maestra de fábrica: solo se usa mientras el ESP32 no haya recibido
// ninguna lista del servidor. Después manda la lista guardada en memoria.
const char* TARJETA_MAESTRA = "23EECDA6";

const char* ssid        = "SAMSUNG";
const char* password    = "12344321";
const char* mqtt_server = "";
const char* mqtt_user   = "esp32";
const char* mqtt_pass   = "";

WiFiClient espClient;
PubSubClient client(espClient);

// RST se maneja a mano (un solo reset para todos) para que ningún lector
// resetee a los demás al inicializarse.
MFRC522 lectores[NUM_LECTORES] = {
  MFRC522(SS_PINS[0], MFRC522::UNUSED_PIN),
  MFRC522(SS_PINS[1], MFRC522::UNUSED_PIN),
  MFRC522(SS_PINS[2], MFRC522::UNUSED_PIN),
  MFRC522(SS_PINS[3], MFRC522::UNUSED_PIN)
};

TaskHandle_t hTareaOnline  = NULL;
TaskHandle_t hTareaOffline = NULL;

volatile bool modoOnline = false;
volatile int ultimaPuerta = 0;
#define TIMEOUT_RESPUESTA_MS 5000
volatile unsigned long pendienteDesde[NUM_LECTORES] = {0};   // millis() del envío al servidor, por puerta (0 = nada pendiente)

volatile unsigned long apagarVerdeEn[NUM_LECTORES] = {0};
volatile unsigned long apagarRojoEn[NUM_LECTORES] = {0};
unsigned long ultimaLectura[NUM_LECTORES] = {0};

// ---------- Tarjetas maestras (guardadas en memoria flash) ----------
Preferences prefs;
String maestras[MAX_MAESTRAS];
int numMaestras = 0;

// Convierte "UID1,UID2,UID3" en el arreglo en RAM.
void cargarEnRam(const String &lista) {
  numMaestras = 0;
  int inicio = 0;
  while (inicio <= (int)lista.length() && numMaestras < MAX_MAESTRAS) {
    int coma = lista.indexOf(',', inicio);
    if (coma < 0) coma = lista.length();
    String uid = lista.substring(inicio, coma);
    uid.trim();
    if (uid.length() > 0) maestras[numMaestras++] = uid;
    inicio = coma + 1;
  }
}

// Lee la lista guardada en flash al arrancar. Si nunca se guardó una, usa la de fábrica.
void cargarMaestras() {
  prefs.begin("acceso", false);
  String lista = prefs.isKey("maestras") ? prefs.getString("maestras", "") : String(TARJETA_MAESTRA);
  prefs.end();
  cargarEnRam(lista);
  Serial.printf("[MAESTRAS] %d tarjeta(s) en memoria.\n", numMaestras);
}

// Guarda la lista en flash solo si cambió (evita desgastar la memoria). Devuelve true si hubo cambio.
bool guardarMaestras(const String &lista) {
  prefs.begin("acceso", false);
  bool cambio = !prefs.isKey("maestras") || prefs.getString("maestras", "") != lista;
  if (cambio) prefs.putString("maestras", lista);
  prefs.end();
  return cambio;
}

// Devuelve true si el UID está en la lista de tarjetas maestras.
bool esMaestra(const String &uid) {
  for (int i = 0; i < numMaestras; i++) {
    if (uid.equalsIgnoreCase(maestras[i])) return true;
  }
  return false;
}

// Procesa el mensaje del servidor con la lista de maestras. Formato: "M:UID1,UID2" ("M:" solo = sin maestras).
void procesarMaestras(byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();
  if (!msg.startsWith("M:")) return;

  String lista = msg.substring(2);
  lista.trim();
  lista.toUpperCase();

  bool cambio = guardarMaestras(lista);
  cargarEnRam(lista);
  if (cambio) {
    Serial.printf("[MAESTRAS] Lista actualizada desde el servidor: %d tarjeta(s).\n", numMaestras);
  }
}

// ---------- WiFi configurable desde el monitor serie (se guarda en flash) ----------
String wifiSsid;
String wifiPass;
SemaphoreHandle_t mtxWifi;                 // protege wifiSsid / wifiPass entre tareas
volatile bool forzarReintentoWifi = false;

// Carga el WiFi guardado en flash. Si nunca se configuró, usa el del código (ssid / password).
void cargarWifi() {
  prefs.begin("acceso", false);
  String s = prefs.isKey("ssid") ? prefs.getString("ssid", "") : String(ssid);
  String p = prefs.isKey("pass") ? prefs.getString("pass", "") : String(password);
  prefs.end();

  xSemaphoreTake(mtxWifi, portMAX_DELAY);
  wifiSsid = s;
  wifiPass = p;
  xSemaphoreGive(mtxWifi);
}

// Guarda el WiFi actual en flash.
void guardarWifi() {
  xSemaphoreTake(mtxWifi, portMAX_DELAY);
  String s = wifiSsid;
  String p = wifiPass;
  xSemaphoreGive(mtxWifi);

  prefs.begin("acceso", false);
  prefs.putString("ssid", s);
  prefs.putString("pass", p);
  prefs.end();
}

// Conecta al WiFi con los datos actuales.
void iniciarWifi() {
  xSemaphoreTake(mtxWifi, portMAX_DELAY);
  String s = wifiSsid;
  String p = wifiPass;
  xSemaphoreGive(mtxWifi);

  WiFi.begin(s.c_str(), p.c_str());
}

// Tras cambiar el WiFi: lo guarda y fuerza una reconexión inmediata con los datos nuevos.
void aplicarWifi() {
  guardarWifi();
  forzarReintentoWifi = true;
  WiFi.disconnect();   // si estaba online, TareaOnline pasa a offline y reintenta ya con los datos nuevos
  Serial.println("[WIFI] Datos guardados. Reconectando...");
}

// Muestra el WiFi configurado y el estado de la conexión (la clave no se muestra).
void mostrarWifi() {
  xSemaphoreTake(mtxWifi, portMAX_DELAY);
  String s = wifiSsid;
  int largoClave = wifiPass.length();
  xSemaphoreGive(mtxWifi);

  String clave = largoClave ? "(" + String(largoClave) + " caracteres)" : String("(sin clave)");
  String estado = (WiFi.status() == WL_CONNECTED) ? "conectado, IP " + WiFi.localIP().toString()
                                                  : String("desconectado");
  Serial.printf("[WIFI] Red: \"%s\" | Clave: %s | Estado: %s | Modo: %s\n",
                s.c_str(), clave.c_str(), estado.c_str(), modoOnline ? "ONLINE" : "OFFLINE");
}

void mostrarAyuda() {
  Serial.println("---- Comandos (termina con Enter; el monitor serie debe estar en 'Nueva línea') ----");
  Serial.println("ssid NOMBRE    cambia el nombre de la red WiFi (admite espacios)");
  Serial.println("pass CLAVE     cambia la clave (sin CLAVE = red abierta)");
  Serial.println("wifi           muestra la red configurada y el estado de la conexión");
  Serial.println("resetwifi      vuelve al WiFi escrito en el código");
  Serial.println("ayuda          muestra esta lista");
}

// Interpreta una línea escrita en el monitor serie.
void procesarLinea(String linea) {
  linea.trim();
  if (linea.length() == 0) return;

  String cmd = linea;
  String arg = "";
  int sp = linea.indexOf(' ');
  if (sp > 0) {
    cmd = linea.substring(0, sp);
    arg = linea.substring(sp + 1);
    arg.trim();
  }
  cmd.toLowerCase();

  if (cmd == "ssid") {
    if (arg.length() == 0) {
      Serial.println("[WIFI] Uso: ssid NOMBRE_DE_LA_RED");
      return;
    }
    xSemaphoreTake(mtxWifi, portMAX_DELAY);
    wifiSsid = arg;
    xSemaphoreGive(mtxWifi);
    Serial.printf("[WIFI] Red cambiada a \"%s\".\n", arg.c_str());
    aplicarWifi();

  } else if (cmd == "pass") {
    xSemaphoreTake(mtxWifi, portMAX_DELAY);
    wifiPass = arg;
    xSemaphoreGive(mtxWifi);
    Serial.println(arg.length() ? "[WIFI] Clave cambiada." : "[WIFI] Clave vacía (red abierta).");
    aplicarWifi();

  } else if (cmd == "wifi") {
    mostrarWifi();

  } else if (cmd == "resetwifi") {
    prefs.begin("acceso", false);
    prefs.remove("ssid");
    prefs.remove("pass");
    prefs.end();
    cargarWifi();
    forzarReintentoWifi = true;
    WiFi.disconnect();
    Serial.println("[WIFI] Restaurado el WiFi escrito en el código. Reconectando...");

  } else if (cmd == "ayuda" || cmd == "help") {
    mostrarAyuda();

  } else {
    Serial.println("[AYUDA] Comando no reconocido. Escribe: ayuda");
  }
}

// ---------- Accesos ocurridos en modo OFFLINE (se envían al volver a ONLINE) ----------
struct EventoOff {
  uint8_t puerta;
  char uid[24];
  bool concedido;
  unsigned long ms;   // millis() en el momento del acceso
};

EventoOff eventosOff[MAX_EVENTOS_OFF];
int numEventosOff = 0;

// Guarda un acceso hecho sin conexión. Si el buffer se llena, descarta el más antiguo.
void registrarEventoOffline(int puerta, const String &uid, bool concedido) {
  if (numEventosOff >= MAX_EVENTOS_OFF) {
    for (int i = 1; i < MAX_EVENTOS_OFF; i++) eventosOff[i - 1] = eventosOff[i];
    numEventosOff = MAX_EVENTOS_OFF - 1;
  }
  EventoOff &e = eventosOff[numEventosOff++];
  e.puerta = puerta;
  uid.toCharArray(e.uid, sizeof(e.uid));
  e.concedido = concedido;
  e.ms = millis();
}

// Envía a Node-RED los accesos guardados. Cada uno lleva "hace_ms" (hace cuánto ocurrió)
// porque sin conexión el ESP32 no tiene reloj real.
void enviarEventosOffline() {
  unsigned long ahora = millis();
  int enviados = 0;

  for (int i = 0; i < numEventosOff; i++) {
    String payload = "{\"puerta\":\"" + String(NOMBRES[eventosOff[i].puerta]) +
                     "\",\"uid\":\"" + String(eventosOff[i].uid) +
                     "\",\"concedido\":" + (eventosOff[i].concedido ? "true" : "false") +
                     ",\"hace_ms\":" + String(ahora - eventosOff[i].ms) + "}";
    if (!client.publish("esp32/offline", payload.c_str())) break;
    enviados++;
    client.loop();
  }

  if (enviados > 0) {
    for (int i = enviados; i < numEventosOff; i++) eventosOff[i - enviados] = eventosOff[i];
    numEventosOff -= enviados;
    Serial.printf("[SYNC] %d acceso(s) offline enviados al servidor.\n", enviados);
  }
}

// Enciende el LED verde de la puerta indicada y programa su apagado tras los milisegundos dados.
void encenderVerde(int idx, unsigned long ms) {
  digitalWrite(LED_VERDE[idx], HIGH);
  apagarVerdeEn[idx] = millis() + ms;
}

// Enciende el LED rojo de la puerta indicada y programa su apagado tras los milisegundos dados.
void encenderRojo(int idx, unsigned long ms) {
  digitalWrite(LED_ROJO[idx], HIGH);
  apagarRojoEn[idx] = millis() + ms;
}

// Revisa los temporizadores de todos los LEDs y apaga los que ya cumplieron su tiempo.
void actualizarLeds() {
  unsigned long ahora = millis();
  for (int i = 0; i < NUM_LECTORES; i++) {
    if (apagarVerdeEn[i] && (long)(ahora - apagarVerdeEn[i]) >= 0) {
      digitalWrite(LED_VERDE[i], LOW);
      apagarVerdeEn[i] = 0;
    }
    if (apagarRojoEn[i] && (long)(ahora - apagarRojoEn[i]) >= 0) {
      digitalWrite(LED_ROJO[i], LOW);
      apagarRojoEn[i] = 0;
    }
  }
}

// Devuelve el índice de la puerta según su nombre, o -1 si no existe.
int buscarPuerta(const String &nombre) {
  for (int i = 0; i < NUM_LECTORES; i++) {
    if (nombre.equalsIgnoreCase(NOMBRES[i])) return i;
  }
  return -1;
}

// Procesa los mensajes MQTT: la lista de maestras (esp32/maestras) o la respuesta del servidor (esp32/respuesta).
void callback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, "esp32/maestras") == 0) {
    procesarMaestras(payload, length);
    return;
  }

  String mensaje = "";
  for (unsigned int i = 0; i < length; i++) mensaje += (char)payload[i];
  mensaje.trim();

  Serial.print("[MQTT] Respuesta de Node-RED: ");
  Serial.println(mensaje);

  String estado = mensaje;
  int idx = ultimaPuerta;

  int sep = mensaje.indexOf(':');
  if (sep > 0) {
    estado = mensaje.substring(0, sep);
    int encontrada = buscarPuerta(mensaje.substring(sep + 1));
    if (encontrada >= 0) idx = encontrada;
  }

  if (pendienteDesde[idx]) {
    Serial.printf("[LATENCIA] Puerta %s: el servidor respondió en %lu ms.\n",
                  NOMBRES[idx], millis() - pendienteDesde[idx]);
    pendienteDesde[idx] = 0;
  }

  if (estado.startsWith("permitido")) {
    Serial.printf("[ACCESO] Concedido en puerta %s. Abriendo...\n", NOMBRES[idx]);
    encenderVerde(idx, 3000);
  } else {
    Serial.printf("[ACCESO] Denegado en puerta %s.\n", NOMBRES[idx]);
    encenderRojo(idx, 3000);
  }
}

// Recorre los lectores buscando una tarjeta nueva; si la encuentra, devuelve su UID en hexadecimal y la puerta donde se leyó.
bool leerTarjeta(int &idx, String &uid) {
  for (int i = 0; i < NUM_LECTORES; i++) {
    if (millis() - ultimaLectura[i] < 2000) continue;

    // Antena secuencial: solo se enciende la del lector que se consulta, así no se interfieren
    lectores[i].PCD_AntennaOn();
    delay(8);   // tiempo para que el campo se estabilice y la tarjeta arranque
    bool hayTarjeta = lectores[i].PICC_IsNewCardPresent() && lectores[i].PICC_ReadCardSerial();

    if (hayTarjeta) {
      ultimaLectura[i] = millis();

      uid = "";
      for (byte b = 0; b < lectores[i].uid.size; b++) {
        if (lectores[i].uid.uidByte[b] < 0x10) uid += "0";
        uid += String(lectores[i].uid.uidByte[b], HEX);
      }
      uid.toUpperCase();

      lectores[i].PICC_HaltA();
      lectores[i].PCD_StopCrypto1();
      lectores[i].PCD_AntennaOff();

      idx = i;
      ultimaPuerta = i;
      Serial.printf("[RFID] Puerta %s - Tarjeta: %s\n", NOMBRES[i], uid.c_str());
      return true;
    }

    lectores[i].PCD_AntennaOff();
  }
  return false;
}

// Cambia el sistema a modo OFFLINE: desconecta MQTT, despierta la tarea offline y suspende la tarea online.
void pasarAOffline(const char* motivo) {
  client.disconnect();
  modoOnline = false;
  for (int i = 0; i < NUM_LECTORES; i++) pendienteDesde[i] = 0;   // las respuestas pendientes ya no llegarán
  Serial.printf("[DESCONEXION] Se perdió %s. >>> MODO OFFLINE (solo tarjeta maestra) <<<\n", motivo);
  vTaskResume(hTareaOffline);
  vTaskSuspend(NULL);
}

// Cambia el sistema a modo ONLINE: despierta la tarea online y suspende la tarea offline.
void pasarAOnline() {
  modoOnline = true;
  Serial.println("[CONEXION] MQTT conectado. >>> MODO ONLINE (valida el servidor) <<<");
  vTaskResume(hTareaOnline);
  vTaskSuspend(NULL);
}

// Tarea del modo online: vigila la conexión, atiende MQTT, sincroniza los accesos offline y envía al servidor cada tarjeta leída para que la valide.
void TareaOnline(void *pvParameters) {
  vTaskSuspend(NULL);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      pasarAOffline("el WiFi");
    } else if (!client.connected()) {
      pasarAOffline("la conexión MQTT");
    }

    client.loop();

    if (numEventosOff > 0) enviarEventosOffline();

    // Si el servidor no responde a una puerta en 5 s, no la dejamos "verificando": rojo y aviso
    for (int i = 0; i < NUM_LECTORES; i++) {
      if (pendienteDesde[i] && millis() - pendienteDesde[i] > TIMEOUT_RESPUESTA_MS) {
        pendienteDesde[i] = 0;
        Serial.printf("[TIMEOUT] Sin respuesta del servidor en puerta %s. Acceso denegado.\n", NOMBRES[i]);
        encenderRojo(i, 3000);
      }
    }

    int idx;
    String uid;
    if (leerTarjeta(idx, uid)) {
      String payload = "{\"puerta\":\"" + String(NOMBRES[idx]) +
                       "\", \"uid\":\"" + uid + "\"}";
      if (client.publish("esp32/rfid", payload.c_str())) {
        pendienteDesde[idx] = millis();
        Serial.printf("[MQTT] Puerta %s: datos enviados para validación.\n", NOMBRES[idx]);
      } else {
        Serial.printf("[MQTT] ERROR: no se pudo enviar la tarjeta de la puerta %s.\n", NOMBRES[idx]);
        encenderRojo(idx, 2000);
      }
    }

    actualizarLeds();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Tarea del modo offline: solo acepta las tarjetas maestras guardadas, registra cada acceso e intenta reconectar WiFi y MQTT cada 10 segundos.
void TareaOffline(void *pvParameters) {
  unsigned long ultimoIntento = 0;

  for (;;) {
    int idx;
    String uid;
    if (leerTarjeta(idx, uid)) {
      if (esMaestra(uid)) {
        Serial.printf("[OFFLINE] Tarjeta maestra. Acceso concedido en puerta %s.\n", NOMBRES[idx]);
        encenderVerde(idx, 3000);
        registrarEventoOffline(idx, uid, true);
      } else {
        Serial.printf("[OFFLINE] Tarjeta no autorizada en puerta %s. Acceso denegado.\n", NOMBRES[idx]);
        encenderRojo(idx, 2000);
        registrarEventoOffline(idx, uid, false);
      }
    }

    bool forzar = forzarReintentoWifi;
    if (forzar || millis() - ultimoIntento > 10000) {
      ultimoIntento = millis();

      if (forzar) {
        forzarReintentoWifi = false;
        WiFi.disconnect();
        Serial.println("[CONEXION] Conectando a WiFi con los datos nuevos...");
        iniciarWifi();
      } else if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[CONEXION] Reintentando WiFi...");
        iniciarWifi();
      } else {
        if (espClient.connect(mqtt_server, 1883, 3000)) {
          espClient.stop();
          if (client.connect("ESP32_Lector_Norte", mqtt_user, mqtt_pass)) {
            client.subscribe("esp32/respuesta");
            client.subscribe("esp32/maestras", 1);
            pasarAOnline();
          } else {
            Serial.printf("[CONEXION] MQTT fallo, rc=%d\n", client.state());
          }
        } else {
          espClient.stop();
          Serial.println("[CONEXION] Broker inalcanzable.");
        }
      }
    }

    actualizarLeds();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Inicializa pines, lectores RFID, MQTT y WiFi, y crea las tareas online y offline arrancando en modo offline.
void setup() {
  Serial.begin(115200);

  mtxWifi = xSemaphoreCreateMutex();
  cargarWifi();
  cargarMaestras();

  for (int i = 0; i < NUM_LECTORES; i++) {
    pinMode(LED_VERDE[i], OUTPUT);
    pinMode(LED_ROJO[i], OUTPUT);
    digitalWrite(LED_VERDE[i], LOW);
    digitalWrite(LED_ROJO[i], LOW);
    pinMode(SS_PINS[i], OUTPUT);
    digitalWrite(SS_PINS[i], HIGH);
  }

  SPI.begin();

  // Un único reset por hardware para todos los lectores
  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, LOW);
  delay(10);
  digitalWrite(RST_PIN, HIGH);
  delay(50);

  for (int i = 0; i < NUM_LECTORES; i++) {
    lectores[i].PCD_Init();

    // Ganancia de 33 dB (la de fábrica): con 48 dB el receptor se satura y casi no lee el UID
    byte v = lectores[i].PCD_ReadRegister(MFRC522::VersionReg);
    lectores[i].PCD_SetAntennaGain(MFRC522::RxGain_33dB);
    lectores[i].PCD_AntennaOff();   // se enciende solo al leer (ver leerTarjeta)

    Serial.printf("[SISTEMA] Lector %s iniciado (version 0x%02X).\n", NOMBRES[i], v);
  }

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  client.setKeepAlive(15);
  client.setSocketTimeout(3);
  client.setBufferSize(512);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // sin ahorro de energía: las respuestas MQTT llegan más rápido
  iniciarWifi();

  Serial.printf("[WIFI] Red configurada: \"%s\"\n", wifiSsid.c_str());
  Serial.println("[SISTEMA] Escribe 'ayuda' para ver los comandos (cambiar el WiFi, etc.).");
  Serial.println("[SISTEMA] Iniciando en modo OFFLINE hasta conectar con el servidor.");

  xTaskCreatePinnedToCore(TareaOffline, "TareaOffline", 6144, NULL, 1, &hTareaOffline, 1);
  xTaskCreatePinnedToCore(TareaOnline,  "TareaOnline",  8192, NULL, 1, &hTareaOnline,  1);
}

String lineaSerial = "";

// Lee los comandos del monitor serie (cambiar WiFi, etc.). El trabajo de acceso lo hacen las tareas de FreeRTOS.
void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      procesarLinea(lineaSerial);
      lineaSerial = "";
    } else if (lineaSerial.length() < 120) {
      lineaSerial += c;
    }
  }
  delay(20);
}
