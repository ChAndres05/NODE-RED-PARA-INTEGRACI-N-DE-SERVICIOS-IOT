#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Preferences.h>

#define RST_PIN 4
#define NUM_LECTORES 4
#define MAX_MAESTRAS 10
#define MAX_EVENTOS_OFF 30
#define T_ESTABILIZAR_MS 8   // tiempo para que el campo de la antena se estabilice antes de leer

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
volatile unsigned long tUltimoEnvio = 0;   // millis() cuando se mandó la última tarjeta al servidor

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
  Serial.printf("[LATENCIA] El servidor respondió en %lu ms.\n", millis() - tUltimoEnvio);

  String estado = mensaje;
  int idx = ultimaPuerta;

  int sep = mensaje.indexOf(':');
  if (sep > 0) {
    estado = mensaje.substring(0, sep);
    int encontrada = buscarPuerta(mensaje.substring(sep + 1));
    if (encontrada >= 0) idx = encontrada;
  }

  if (estado.startsWith("permitido")) {
    Serial.printf("[ACCESO] Concedido en puerta %s. Abriendo...\n", NOMBRES[idx]);
    encenderVerde(idx, 3000);
  } else {
    Serial.printf("[ACCESO] Denegado en puerta %s.\n", NOMBRES[idx]);
    encenderRojo(idx, 3000);
  }
}

// Recorre los lectores de forma SECUENCIAL: enciende la antena de uno, espera a que el campo
// se estabilice, intenta leer y la apaga antes de pasar al siguiente. Así las antenas vecinas
// no se interfieren. Si encuentra una tarjeta, devuelve su UID en hexadecimal y la puerta donde se leyó.
bool leerTarjeta(int &idx, String &uid) {
  for (int i = 0; i < NUM_LECTORES; i++) {
    if (millis() - ultimaLectura[i] < 2000) continue;

    lectores[i].PCD_AntennaOn();
    vTaskDelay(pdMS_TO_TICKS(T_ESTABILIZAR_MS));

    bool ok = lectores[i].PICC_IsNewCardPresent() && lectores[i].PICC_ReadCardSerial();

    if (ok) {
      uid = "";
      for (byte b = 0; b < lectores[i].uid.size; b++) {
        if (lectores[i].uid.uidByte[b] < 0x10) uid += "0";
        uid += String(lectores[i].uid.uidByte[b], HEX);
      }
      uid.toUpperCase();

      lectores[i].PICC_HaltA();
      lectores[i].PCD_StopCrypto1();
    }

    lectores[i].PCD_AntennaOff();

    if (ok) {
      ultimaLectura[i] = millis();
      idx = i;
      ultimaPuerta = i;
      Serial.printf("[RFID] Puerta %s - Tarjeta: %s\n", NOMBRES[i], uid.c_str());
      return true;
    }
  }
  return false;
}

// Cambia el sistema a modo OFFLINE: desconecta MQTT, despierta la tarea offline y suspende la tarea online.
void pasarAOffline(const char* motivo) {
  client.disconnect();
  modoOnline = false;
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

    int idx;
    String uid;
    if (leerTarjeta(idx, uid)) {
      String payload = "{\"puerta\":\"" + String(NOMBRES[idx]) +
                       "\", \"uid\":\"" + uid + "\"}";
      client.publish("esp32/rfid", payload.c_str());
      tUltimoEnvio = millis();
      Serial.println("[MQTT] Datos enviados para validación.");
    }

    actualizarLeds();
    vTaskDelay(pdMS_TO_TICKS(20));   // antes 50: el ciclo secuencial ya consume ~45 ms
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

    if (millis() - ultimoIntento > 10000) {
      ultimoIntento = millis();

      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[CONEXION] Reintentando WiFi...");
        WiFi.begin(ssid, password);
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
    vTaskDelay(pdMS_TO_TICKS(20));   // antes 50: el ciclo secuencial ya consume ~45 ms
  }
}

// Inicializa pines, lectores RFID, MQTT y WiFi, y crea las tareas online y offline arrancando en modo offline.
void setup() {
  Serial.begin(115200);

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

    // Lectura secuencial: todas las antenas apagadas; leerTarjeta() enciende una a la vez
    lectores[i].PCD_AntennaOff();

    Serial.printf("[SISTEMA] Lector %s iniciado (version 0x%02X).\n", NOMBRES[i], v);
  }

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  client.setKeepAlive(15);
  client.setSocketTimeout(3);
  client.setBufferSize(512);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // sin ahorro de energía: las respuestas MQTT llegan más rápido
  WiFi.begin(ssid, password);

  Serial.println("[SISTEMA] Iniciando en modo OFFLINE hasta conectar con el servidor.");

  xTaskCreatePinnedToCore(TareaOffline, "TareaOffline", 6144, NULL, 1, &hTareaOffline, 1);
  xTaskCreatePinnedToCore(TareaOnline,  "TareaOnline",  8192, NULL, 1, &hTareaOnline,  1);
}

// No se usa: el trabajo lo hacen las tareas de FreeRTOS, así que este hilo se elimina.
void loop() {
  vTaskDelete(NULL);
}
