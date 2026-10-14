#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>

// ---------- Pines ----------
#define RST_PIN 4         
#define NUM_LECTORES 4

//                          rfid1    rfid2    rfid4    rfid3
const char*   NOMBRES[]  = {"norte", "sur",   "este",  "oeste"};
const uint8_t SS_PINS[]  = {5,       21,      27,      14};
const uint8_t LED_VERDE[] = {12,     25,      32,      16};
const uint8_t LED_ROJO[]  = {13,     26,      33,      17};

// ---------- Red ----------
const char* ssid = "Wokwi-GUEST";
const char* password = "";
const char* mqtt_server = "3.133.117.11";
const char* mqtt_user = "esp32";
const char* mqtt_pass = "Telematicos2026-1";

WiFiClient espClient;
PubSubClient client(espClient);

MFRC522 lectores[NUM_LECTORES] = {
  MFRC522(SS_PINS[0], RST_PIN),
  MFRC522(SS_PINS[1], RST_PIN),
  MFRC522(SS_PINS[2], RST_PIN),
  MFRC522(SS_PINS[3], RST_PIN)
};

// ---------- Estado compartido ----------
struct EventoRFID {
  uint8_t idx;
  char uid[24];
};

QueueHandle_t colaRFID;
volatile bool mqttConectado = false;
volatile int ultimaPuerta = 0;

volatile unsigned long apagarVerdeEn[NUM_LECTORES] = {0};
volatile unsigned long apagarRojoEn[NUM_LECTORES] = {0};
unsigned long ultimaLectura[NUM_LECTORES] = {0};

// ---------- Utilidades LEDs ----------
void encenderVerde(int idx, unsigned long ms) {
  digitalWrite(LED_VERDE[idx], HIGH);
  apagarVerdeEn[idx] = millis() + ms;
}

void encenderRojo(int idx, unsigned long ms) {
  digitalWrite(LED_ROJO[idx], HIGH);
  apagarRojoEn[idx] = millis() + ms;
}

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

int buscarPuerta(const String &nombre) {
  for (int i = 0; i < NUM_LECTORES; i++) {
    if (nombre.equalsIgnoreCase(NOMBRES[i])) return i;
  }
  return -1;
}

// ---------- MQTT ----------
// Formatos aceptados: "permitido:sur", "denegado:sur"
// Si no viene la puerta, se usa la última puerta que escaneó.
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

void TareaMQTT(void *pvParameters) {
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  client.setKeepAlive(60);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      mqttConectado = false;
      Serial.println("[SISTEMA] Conectando a WiFi...");
      WiFi.begin(ssid, password);
      while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
      }
      Serial.println("[SISTEMA] WiFi Conectado.");
    }

    if (!client.connected()) {
      mqttConectado = false;
      Serial.println("[SISTEMA] Intentando conectar a MQTT...");
      if (client.connect("ESP32_Lector_Norte", mqtt_user, mqtt_pass)) {
        Serial.println("[SISTEMA] Conectado. Activando Modo Online.");
        client.subscribe("esp32/respuesta");
        mqttConectado = true;
      } else {
        
        Serial.printf("[ALERTA] MQTT fallo, rc=%d. Reintentando en 5s...\n", client.state());
        vTaskDelay(5000 / portTICK_PERIOD_MS);
      }
    } else {
      client.loop();

      
      EventoRFID ev;
      while (xQueueReceive(colaRFID, &ev, 0) == pdTRUE) {
        String payload = "{\"puerta\":\"" + String(NOMBRES[ev.idx]) +
                         "\", \"uid\":\"" + String(ev.uid) + "\"}";
        client.publish("esp32/rfid", payload.c_str());
        Serial.println("[MQTT] Datos enviados para validación.");
      }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// ---------- RFID ----------
void TareaRFID(void *pvParameters) {
  SPI.begin();

 
  for (int i = 0; i < NUM_LECTORES; i++) {
    pinMode(SS_PINS[i], OUTPUT);
    digitalWrite(SS_PINS[i], HIGH);
  }

  for (int i = 0; i < NUM_LECTORES; i++) {
    lectores[i].PCD_Init();
    Serial.printf("[SISTEMA] Lector %s iniciado.\n", NOMBRES[i]);
  }
  Serial.println("[SISTEMA] Acerque una tarjeta...");

  for (;;) {
    for (int i = 0; i < NUM_LECTORES; i++) {
      // Enfriamiento de 2 s por lector, sin bloquear a los demás
      if (millis() - ultimaLectura[i] < 2000) continue;

      if (lectores[i].PICC_IsNewCardPresent() && lectores[i].PICC_ReadCardSerial()) {
        ultimaLectura[i] = millis();

        String uid = "";
        for (byte b = 0; b < lectores[i].uid.size; b++) {
          if (lectores[i].uid.uidByte[b] < 0x10) uid += "0";
          uid += String(lectores[i].uid.uidByte[b], HEX);
        }
        uid.toUpperCase();

        Serial.printf("[RFID] Puerta %s - Tarjeta detectada: %s\n", NOMBRES[i], uid.c_str());

        lectores[i].PICC_HaltA();
        lectores[i].PCD_StopCrypto1();

        ultimaPuerta = i;

        if (mqttConectado) {
          EventoRFID ev;
          ev.idx = i;
          uid.toCharArray(ev.uid, sizeof(ev.uid));
          xQueueSend(colaRFID, &ev, 0);
        } else {
          Serial.println("[ALERTA] Sin conexión. Acceso denegado temporalmente.");
          encenderRojo(i, 1000);
        }
      }
    }

    actualizarLeds();
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < NUM_LECTORES; i++) {
    pinMode(LED_VERDE[i], OUTPUT);
    pinMode(LED_ROJO[i], OUTPUT);
    digitalWrite(LED_VERDE[i], LOW);
    digitalWrite(LED_ROJO[i], LOW);
  }

  colaRFID = xQueueCreate(8, sizeof(EventoRFID));

  xTaskCreatePinnedToCore(TareaMQTT, "TareaMQTT", 6144, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TareaRFID, "TareaRFID", 6144, NULL, 1, NULL, 1);
}

void loop() {
  vTaskDelete(NULL);
}
