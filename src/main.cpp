#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>

#define RST_PIN 4
#define NUM_LECTORES 4

const char* NOMBRES[]     = {"norte", "sur",   "este",  "oeste"};
const uint8_t SS_PINS[]   = {5,       21,      27,      14};
const uint8_t LED_VERDE[] = {15,      25,      32,      16};
const uint8_t LED_ROJO[]  = {13,      26,      33,      17};

const char* TARJETA_MAESTRA = "23EECDA6";

const char* ssid        = "SAMSUNG";
const char* password    = "12344321";
const char* mqtt_server = "3.133.117.11";
const char* mqtt_user   = "esp32";
const char* mqtt_pass   = "estoycansado";

WiFiClient espClient;
PubSubClient client(espClient);

MFRC522 lectores[NUM_LECTORES] = {
  MFRC522(SS_PINS[0], RST_PIN),
  MFRC522(SS_PINS[1], RST_PIN),
  MFRC522(SS_PINS[2], RST_PIN),
  MFRC522(SS_PINS[3], RST_PIN)
};

TaskHandle_t hTareaOnline  = NULL;
TaskHandle_t hTareaOffline = NULL;

volatile bool modoOnline = false;
volatile int ultimaPuerta = 0;

volatile unsigned long apagarVerdeEn[NUM_LECTORES] = {0};
volatile unsigned long apagarRojoEn[NUM_LECTORES] = {0};
unsigned long ultimaLectura[NUM_LECTORES] = {0};

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

// Procesa la respuesta MQTT del servidor y enciende el LED verde (permitido) o rojo (denegado) en la puerta correspondiente.
void callback(char* topic, byte* payload, unsigned int length) {
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

    if (lectores[i].PICC_IsNewCardPresent() && lectores[i].PICC_ReadCardSerial()) {
      ultimaLectura[i] = millis();

      uid = "";
      for (byte b = 0; b < lectores[i].uid.size; b++) {
        if (lectores[i].uid.uidByte[b] < 0x10) uid += "0";
        uid += String(lectores[i].uid.uidByte[b], HEX);
      }
      uid.toUpperCase();

      lectores[i].PICC_HaltA();
      lectores[i].PCD_StopCrypto1();

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

// Tarea del modo online: vigila la conexión, atiende MQTT y envía al servidor cada tarjeta leída para que la valide.
void TareaOnline(void *pvParameters) {
  vTaskSuspend(NULL);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      pasarAOffline("el WiFi");
    } else if (!client.connected()) {
      pasarAOffline("la conexión MQTT");
    }

    client.loop();

    int idx;
    String uid;
    if (leerTarjeta(idx, uid)) {
      String payload = "{\"puerta\":\"" + String(NOMBRES[idx]) +
                       "\", \"uid\":\"" + uid + "\"}";
      client.publish("esp32/rfid", payload.c_str());
      Serial.println("[MQTT] Datos enviados para validación.");
    }

    actualizarLeds();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Tarea del modo offline: solo acepta la tarjeta maestra e intenta reconectar WiFi y MQTT cada 10 segundos.
void TareaOffline(void *pvParameters) {
  unsigned long ultimoIntento = 0;

  for (;;) {
    int idx;
    String uid;
    if (leerTarjeta(idx, uid)) {
      if (uid.equalsIgnoreCase(TARJETA_MAESTRA)) {
        Serial.printf("[OFFLINE] Tarjeta maestra. Acceso concedido en puerta %s.\n", NOMBRES[idx]);
        encenderVerde(idx, 3000);
      } else {
        Serial.printf("[OFFLINE] Tarjeta no autorizada en puerta %s. Acceso denegado.\n", NOMBRES[idx]);
        encenderRojo(idx, 2000);
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

  for (int i = 0; i < NUM_LECTORES; i++) {
    pinMode(LED_VERDE[i], OUTPUT);
    pinMode(LED_ROJO[i], OUTPUT);
    digitalWrite(LED_VERDE[i], LOW);
    digitalWrite(LED_ROJO[i], LOW);
    pinMode(SS_PINS[i], OUTPUT);
    digitalWrite(SS_PINS[i], HIGH);
  }

  SPI.begin();
  for (int i = 0; i < NUM_LECTORES; i++) {
    lectores[i].PCD_Init();
    Serial.printf("[SISTEMA] Lector %s iniciado.\n", NOMBRES[i]);
  }

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  client.setKeepAlive(15);
  client.setSocketTimeout(3);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.println("[SISTEMA] Iniciando en modo OFFLINE hasta conectar con el servidor.");

  xTaskCreatePinnedToCore(TareaOffline, "TareaOffline", 6144, NULL, 1, &hTareaOffline, 1);
  xTaskCreatePinnedToCore(TareaOnline,  "TareaOnline",  6144, NULL, 1, &hTareaOnline,  1);
}

// No se usa: el trabajo lo hacen las tareas de FreeRTOS, así que este hilo se elimina.
void loop() {
  vTaskDelete(NULL);
}
