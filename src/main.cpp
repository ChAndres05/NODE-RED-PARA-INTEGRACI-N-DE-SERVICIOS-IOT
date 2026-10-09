#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>

// Definición de pines (Coinciden con tu diagrama de Wokwi)
#define RST_PIN         4
#define SS_PIN          5
#define LED_VERDE       12
#define LED_ROJO        13

// Credenciales WiFi y Servidor
const char* ssid = "Wokwi-GUEST";
const char* password = "";
const char* mqtt_server = "146.181.17.135"; // IP de tu VPS

WiFiClient espClient;
PubSubClient client(espClient);
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Función que se ejecuta cuando Node-RED responde
void callback(char* topic, byte* payload, unsigned int length) {
  String mensaje = "";
  for (int i = 0; i < length; i++) {
    mensaje += (char)payload[i];
  }
  
  Serial.print("[MQTT] Respuesta de Node-RED: ");
  Serial.println(mensaje);

  // Evaluamos la respuesta ("permitido,norte" o "denegado,norte")
  if (mensaje.startsWith("permitido")) {
    Serial.println("[ACCESO] Concedido. Abriendo puerta...");
    digitalWrite(LED_VERDE, HIGH);
    vTaskDelay(3000 / portTICK_PERIOD_MS); // Mantiene el LED encendido 3 seg
    digitalWrite(LED_VERDE, LOW);
  } else {
    Serial.println("[ACCESO] Denegado.");
    digitalWrite(LED_ROJO, HIGH);
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    digitalWrite(LED_ROJO, LOW);
  }
}

// TAREA 1: Manejo de Red (Asignada al Núcleo 0)
void TareaMQTT(void *pvParameters) {
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  client.setKeepAlive(60); // Tolerancia de 60 segundos para evitar cortes

  for (;;) {
    // 1. Mantener WiFi conectado
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[SISTEMA] Conectando a WiFi...");
      WiFi.begin(ssid, password);
      while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
      }
      Serial.println("[SISTEMA] WiFi Conectado.");
    }

    // 2. Mantener MQTT conectado
    if (!client.connected()) {
      Serial.println("[SISTEMA] Intentando conectar a MQTT...");
      // Client ID único
      if (client.connect("ESP32_Lector_Norte")) {
        Serial.println("[SISTEMA] Conectado. Activando Modo Online.");
        client.subscribe("esp32/respuesta");
      } else {
        Serial.println("[ALERTA] Conexión perdida. Reintentando en 5s...");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
      }
    } else {
      // 3. Escuchar mensajes sin bloquear el procesador
      client.loop();
    }

    // Respiro vital para el procesador
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// TAREA 2: Lector RFID (Asignada al Núcleo 1)
void TareaRFID(void *pvParameters) {
  SPI.begin();
  mfrc522.PCD_Init();
  Serial.println("[SISTEMA] Lector RFID iniciado. Acerque una tarjeta...");

  for (;;) {
    // Si hay una tarjeta presente y se puede leer
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      String uid = "";
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        uid += String(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
        uid += String(mfrc522.uid.uidByte[i], HEX);
      }
      uid.toUpperCase();
      
      Serial.print("[RFID] Tarjeta detectada: ");
      Serial.println(uid);

      // Armamos un JSON válido para que Node-RED extraiga msg.payload.uid y msg.payload.puerta
      String payload = "{\"puerta\":\"norte\", \"uid\":\"" + uid + "\"}";

      if (client.connected()) {
        client.publish("esp32/rfid", payload.c_str());
        Serial.println("[MQTT] Datos enviados para validación.");
      } else {
        Serial.println("[ALERTA] Sin conexión. Acceso denegado temporalmente.");
        // Aquí puedes agregar tu lógica de "Modo Caché" en el futuro
        digitalWrite(LED_ROJO, HIGH);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        digitalWrite(LED_ROJO, LOW);
      }

      // Evita que la misma tarjeta se lea múltiples veces por segundo
      vTaskDelay(2000 / portTICK_PERIOD_MS); 
    }
    
    // Respiro vital para el procesador
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_ROJO, OUTPUT);
  
  // Aseguramos que los LEDs inicien apagados
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_ROJO, LOW);

  // Creamos la tarea de Red en el Núcleo 0
  xTaskCreatePinnedToCore(TareaMQTT, "TareaMQTT", 4096, NULL, 1, NULL, 0);
  
  // Creamos la tarea de RFID en el Núcleo 1
  xTaskCreatePinnedToCore(TareaRFID, "TareaRFID", 4096, NULL, 1, NULL, 1);
}

void loop() {
  // En FreeRTOS, eliminamos el loop principal para ahorrar memoria
  vTaskDelete(NULL); 
}