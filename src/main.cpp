#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>

#define RST_PIN         4
#define SS_PIN          5
#define LED_VERDE       12
#define LED_ROJO        13

const char* ssid = "Wokwi-GUEST";
const char* password = "";
const char* mqtt_server = "146.181.17.135";

WiFiClient espClient;
PubSubClient client(espClient);
MFRC522 mfrc522(SS_PIN, RST_PIN);

void callback(char* topic, byte* payload, unsigned int length) {
  String mensaje = "";
  for (int i = 0; i < length; i++) {
    mensaje += (char)payload[i];
  }
  
  Serial.print("[MQTT] Respuesta de Node-RED: ");
  Serial.println(mensaje);

  if (mensaje.startsWith("permitido")) {
    Serial.println("[ACCESO] Concedido. Abriendo puerta...");
    digitalWrite(LED_VERDE, HIGH);
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    digitalWrite(LED_VERDE, LOW);
  } else {
    Serial.println("[ACCESO] Denegado.");
    digitalWrite(LED_ROJO, HIGH);
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    digitalWrite(LED_ROJO, LOW);
  }
}

void TareaMQTT(void *pvParameters) {
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  client.setKeepAlive(60);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[SISTEMA] Conectando a WiFi...");
      WiFi.begin(ssid, password);
      while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
      }
      Serial.println("[SISTEMA] WiFi Conectado.");
    }

    if (!client.connected()) {
      Serial.println("[SISTEMA] Intentando conectar a MQTT...");
      if (client.connect("ESP32_Lector_Norte")) {
        Serial.println("[SISTEMA] Conectado. Activando Modo Online.");
        client.subscribe("esp32/respuesta");
      } else {
        Serial.println("[ALERTA] Conexión perdida. Reintentando en 5s...");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
      }
    } else {
      client.loop();
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void TareaRFID(void *pvParameters) {
  SPI.begin();
  mfrc522.PCD_Init();
  Serial.println("[SISTEMA] Lector RFID iniciado. Acerque una tarjeta...");

  for (;;) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      String uid = "";
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        uid += String(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
        uid += String(mfrc522.uid.uidByte[i], HEX);
      }
      uid.toUpperCase();
      
      Serial.print("[RFID] Tarjeta detectada: ");
      Serial.println(uid);

      String payload = "{\"puerta\":\"norte\", \"uid\":\"" + uid + "\"}";

      if (client.connected()) {
        client.publish("esp32/rfid", payload.c_str());
        Serial.println("[MQTT] Datos enviados para validación.");
      } else {
        Serial.println("[ALERTA] Sin conexión. Acceso denegado temporalmente.");
        digitalWrite(LED_ROJO, HIGH);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        digitalWrite(LED_ROJO, LOW);
      }

      vTaskDelay(2000 / portTICK_PERIOD_MS); 
    }
    
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_ROJO, OUTPUT);
  
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_ROJO, LOW);

  xTaskCreatePinnedToCore(TareaMQTT, "TareaMQTT", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TareaRFID, "TareaRFID", 4096, NULL, 1, NULL, 1);
}

void loop() {
  vTaskDelete(NULL); 
}