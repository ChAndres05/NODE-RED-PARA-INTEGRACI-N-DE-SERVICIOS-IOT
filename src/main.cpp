#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Preferences.h>

#define RST_PIN 4
#define NUM_LECTORES 4
#define MAX_MAESTRAS 10
#define MAX_EVENTOS_OFF 30
#define TIMEOUT_RESPUESTA_MS 10000

// Bloqueo por lecturas repetidas de la misma tarjeta en el mismo lector.
#define MAX_REPETICIONES 3            // lecturas seguidas que disparan el bloqueo
#define VENTANA_REPETICION_MS 10000   // máximo tiempo entre lecturas para considerarlas "seguidas"
#define BLOQUEO_REPETICION_MS 60000   // duración del bloqueo (1 minuto)

const char* NOMBRES[]     = {"norte", "sur",   "este",  "oeste"};
const uint8_t SS_PINS[]   = {5,       21,      27,      14};
const uint8_t LED_VERDE[] = {15,      25,      32,      16};
const uint8_t LED_ROJO[]  = {13,      26,      33,      17};

const char* TOPIC_RFID      = "esp32/rfid";
const char* TOPIC_RESPUESTA = "esp32/respuesta";
const char* TOPIC_MAESTRAS  = "esp32/maestras";
const char* TOPIC_OFFLINE   = "esp32/offline";

WiFiClient espClient;
PubSubClient client(espClient);

MFRC522 lectores[NUM_LECTORES] = {
  MFRC522(SS_PINS[0], MFRC522::UNUSED_PIN),
  MFRC522(SS_PINS[1], MFRC522::UNUSED_PIN),
  MFRC522(SS_PINS[2], MFRC522::UNUSED_PIN),
  MFRC522(SS_PINS[3], MFRC522::UNUSED_PIN)
};

TaskHandle_t hTareaOnline  = NULL;
TaskHandle_t hTareaOffline = NULL;

volatile bool modoOnline = false;
volatile bool reintentar = true;
volatile bool wifiNuevo = false;
volatile int ultimaPuerta = 0;

// Si es distinto de 0, el lector está bloqueado esperando la respuesta del servidor (guarda el instante del envío).
volatile unsigned long pendienteDesde[NUM_LECTORES] = {0};

volatile unsigned long apagarVerdeEn[NUM_LECTORES] = {0};
volatile unsigned long apagarRojoEn[NUM_LECTORES] = {0};
unsigned long ultimaLectura[NUM_LECTORES] = {0};

// Control de lecturas repetidas y bloqueo temporal por lector.
String ultimoUid[NUM_LECTORES];
uint8_t repeticiones[NUM_LECTORES] = {0};
unsigned long ultimaRepeticionMs[NUM_LECTORES] = {0};
volatile bool bloqueado[NUM_LECTORES] = {false};
unsigned long bloqueadoHasta[NUM_LECTORES] = {0};

Preferences prefs;
SemaphoreHandle_t mtxConfig;

String wifiSsid;
String wifiPass;
String mqttHost;
String mqttUser;
String mqttPass;
uint16_t mqttPuerto = 1883;
volatile uint8_t gananciaDb = 33;
char hostActual[64];
String idCliente;

String maestras[MAX_MAESTRAS];
int numMaestras = 0;

struct EventoOff {
  uint8_t puerta;
  char uid[24];
  bool concedido;
  unsigned long ms;
};

EventoOff eventosOff[MAX_EVENTOS_OFF];
int numEventosOff = 0;

// Convierte el texto "UID1,UID2,UID3" en el arreglo de tarjetas maestras en RAM.
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

// Lee de la memoria flash la lista de maestras guardada (vacía hasta que el servidor envíe una).
void cargarMaestras() {
  prefs.begin("acceso", false);
  String lista = prefs.getString("maestras", "");
  prefs.end();
  cargarEnRam(lista);
  Serial.printf("[MAESTRAS] %d tarjeta(s) en memoria.\n", numMaestras);
}

// Guarda la lista de maestras en flash solo si cambió y devuelve true cuando hubo cambio.
bool guardarMaestras(const String &lista) {
  prefs.begin("acceso", false);
  bool cambio = !prefs.isKey("maestras") || prefs.getString("maestras", "") != lista;
  if (cambio) prefs.putString("maestras", lista);
  prefs.end();
  return cambio;
}

// Devuelve true si el UID recibido está en la lista de tarjetas maestras.
bool esMaestra(const String &uid) {
  for (int i = 0; i < numMaestras; i++) {
    if (uid.equalsIgnoreCase(maestras[i])) return true;
  }
  return false;
}

// Procesa el mensaje del servidor con la lista de maestras (formato "M:UID1,UID2") y la guarda en flash.
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

// Convierte una ganancia en dB (18, 23, 33, 38, 43 o 48) al valor de registro del lector; devuelve 0xFF si no es válida.
byte codigoGanancia(int db) {
  switch (db) {
    case 18: return MFRC522::RxGain_18dB;
    case 23: return MFRC522::RxGain_23dB;
    case 33: return MFRC522::RxGain_33dB;
    case 38: return MFRC522::RxGain_38dB;
    case 43: return MFRC522::RxGain_43dB;
    case 48: return MFRC522::RxGain_48dB;
    default: return 0xFF;
  }
}

// Carga de la memoria flash toda la configuración de WiFi y de servidor MQTT.
void cargarConfig() {
  prefs.begin("acceso", false);
  String s  = prefs.getString("ssid", "");
  String p  = prefs.getString("pass", "");
  String h  = prefs.getString("host", "");
  String u  = prefs.getString("muser", "");
  String mp = prefs.getString("mpass", "");
  uint16_t pt = prefs.getUShort("port", 1883);
  uint8_t g = prefs.getUChar("gain", 33);
  prefs.end();

  gananciaDb = (codigoGanancia(g) == 0xFF) ? 33 : g;

  xSemaphoreTake(mtxConfig, portMAX_DELAY);
  wifiSsid = s;
  wifiPass = p;
  mqttHost = h;
  mqttUser = u;
  mqttPass = mp;
  mqttPuerto = pt;
  xSemaphoreGive(mtxConfig);
}

// Guarda un valor de texto de la configuración en la memoria flash.
void guardarTexto(const char* clave, const String &valor) {
  prefs.begin("acceso", false);
  prefs.putString(clave, valor);
  prefs.end();
}

// Indica si ya hay un nombre de red WiFi configurado.
bool wifiConfigurado() {
  xSemaphoreTake(mtxConfig, portMAX_DELAY);
  bool ok = wifiSsid.length() > 0;
  xSemaphoreGive(mtxConfig);
  return ok;
}

// Indica si ya hay una dirección de servidor MQTT configurada.
bool servidorConfigurado() {
  xSemaphoreTake(mtxConfig, portMAX_DELAY);
  bool ok = mqttHost.length() > 0;
  xSemaphoreGive(mtxConfig);
  return ok;
}

// Inicia la conexión WiFi con el nombre y la clave guardados.
void iniciarWifi() {
  xSemaphoreTake(mtxConfig, portMAX_DELAY);
  String s = wifiSsid;
  String p = wifiPass;
  xSemaphoreGive(mtxConfig);

  WiFi.begin(s.c_str(), p.c_str());
}

// Cambia un valor de configuración de texto, lo guarda en flash y avisa a las tareas para reconectar.
void cambiarTexto(const char* comando, const char* clave, String &destino, const String &valor, bool esWifi) {
  xSemaphoreTake(mtxConfig, portMAX_DELAY);
  destino = valor;
  xSemaphoreGive(mtxConfig);

  guardarTexto(clave, valor);

  if (esWifi) {
    wifiNuevo = true;
    WiFi.disconnect();
  }
  reintentar = true;
  Serial.printf("[CONFIG] '%s' guardado. Reconectando...\n", comando);
}

// Cambia el puerto del servidor MQTT, lo guarda en flash y avisa a las tareas para reconectar.
void cambiarPuerto(long numero) {
  if (numero < 1 || numero > 65535) {
    Serial.println("[CONFIG] Uso: port NUMERO (entre 1 y 65535)");
    return;
  }

  xSemaphoreTake(mtxConfig, portMAX_DELAY);
  mqttPuerto = (uint16_t)numero;
  xSemaphoreGive(mtxConfig);

  prefs.begin("acceso", false);
  prefs.putUShort("port", (uint16_t)numero);
  prefs.end();

  reintentar = true;
  Serial.printf("[CONFIG] 'port' guardado: %ld. Reconectando...\n", numero);
}

// Cambia la ganancia de recepción de las antenas (menos dB = menos alcance) y la guarda en flash; se aplica en la siguiente lectura.
void cambiarGanancia(long db) {
  if (codigoGanancia(db) == 0xFF) {
    Serial.println("[CONFIG] Uso: ganancia 18, 23, 33, 38, 43 o 48 (menos = menos alcance)");
    return;
  }

  gananciaDb = (uint8_t)db;

  prefs.begin("acceso", false);
  prefs.putUChar("gain", (uint8_t)db);
  prefs.end();

  Serial.printf("[CONFIG] 'ganancia' guardada: %ld dB.\n", db);
}

// Muestra la configuración actual (sin mostrar las claves), el estado de la conexión y las tarjetas maestras.
void mostrarConfig() {
  xSemaphoreTake(mtxConfig, portMAX_DELAY);
  String s = wifiSsid;
  int largoWifi = wifiPass.length();
  String h = mqttHost;
  String u = mqttUser;
  int largoMqtt = mqttPass.length();
  uint16_t pt = mqttPuerto;
  xSemaphoreGive(mtxConfig);

  String claveWifi = largoWifi ? "(" + String(largoWifi) + " caracteres)" : String("(vacía)");
  String claveMqtt = largoMqtt ? "(" + String(largoMqtt) + " caracteres)" : String("(vacía)");
  String estadoWifi = (WiFi.status() == WL_CONNECTED) ? "conectado, IP " + WiFi.localIP().toString()
                                                      : String("desconectado");

  Serial.println("---- Configuración actual ----");
  Serial.printf("ssid     : %s\n", s.length() ? s.c_str() : "(sin configurar)");
  Serial.printf("pass     : %s\n", claveWifi.c_str());
  Serial.printf("server   : %s\n", h.length() ? h.c_str() : "(sin configurar)");
  Serial.printf("port     : %u\n", pt);
  Serial.printf("ganancia : %u dB\n", gananciaDb);
  Serial.printf("mqttuser : %s\n", u.length() ? u.c_str() : "(ninguno)");
  Serial.printf("mqttpass : %s\n", claveMqtt.c_str());
  Serial.printf("WiFi     : %s\n", estadoWifi.c_str());
  Serial.printf("Modo     : %s\n", modoOnline ? "ONLINE" : "OFFLINE");
  Serial.printf("Maestras : %d\n", numMaestras);
  
}

// Muestra la lista de comandos disponibles por el monitor serie.
void mostrarAyuda() {
  Serial.println("---- Comandos (termina con Enter; el monitor serie debe estar en 'Nueva línea') ----");
  Serial.println("ssid NOMBRE      nombre de la red WiFi (admite espacios)");
  Serial.println("pass CLAVE       clave del WiFi (sin CLAVE = red abierta)");
  Serial.println("server DIRECCION IP o dominio del servidor MQTT");
  Serial.println("port NUMERO      puerto del servidor MQTT (por defecto 1883)");
  Serial.println("ganancia DB      alcance de los lectores: 18, 23, 33, 38, 43 o 48 (menos = menos alcance)");
  Serial.println("mqttuser USUARIO usuario MQTT (sin USUARIO = sin autenticación)");
  Serial.println("mqttpass CLAVE   clave MQTT");
  Serial.println("config           muestra la configuración y el estado");
  Serial.println("borrar           borra TODO lo guardado (incluidas las maestras) y reinicia");
  Serial.println("ayuda            muestra esta lista");
}

// Borra de la memoria flash toda la configuración y la lista de maestras, y reinicia el ESP32.
void borrarTodo() {
  prefs.begin("acceso", false);
  prefs.clear();
  prefs.end();
  Serial.println("[CONFIG] Todo borrado. Reiniciando...");
  delay(300);
  ESP.restart();
}

// Interpreta una línea escrita en el monitor serie y ejecuta el comando correspondiente.
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
      Serial.println("[CONFIG] Uso: ssid NOMBRE_DE_LA_RED");
      return;
    }
    cambiarTexto("ssid", "ssid", wifiSsid, arg, true);

  } else if (cmd == "pass") {
    cambiarTexto("pass", "pass", wifiPass, arg, true);

  } else if (cmd == "server") {
    if (arg.length() == 0) {
      Serial.println("[CONFIG] Uso: server DIRECCION_O_IP");
      return;
    }
    cambiarTexto("server", "host", mqttHost, arg, false);

  } else if (cmd == "port") {
    cambiarPuerto(arg.toInt());

  } else if (cmd == "ganancia") {
    cambiarGanancia(arg.toInt());

  } else if (cmd == "mqttuser") {
    cambiarTexto("mqttuser", "muser", mqttUser, arg, false);

  } else if (cmd == "mqttpass") {
    cambiarTexto("mqttpass", "mpass", mqttPass, arg, false);

  } else if (cmd == "config") {
    mostrarConfig();

  } else if (cmd == "borrar") {
    borrarTodo();

  } else if (cmd == "ayuda" || cmd == "help") {
    mostrarAyuda();

  } else {
    Serial.println("[AYUDA] Comando no reconocido. Escribe: ayuda");
  }
}

// Guarda en el buffer un acceso hecho sin conexión, descartando el más antiguo si está lleno.
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

// Envía al servidor los accesos guardados sin conexión, indicando hace cuánto ocurrió cada uno.
void enviarEventosOffline() {
  unsigned long ahora = millis();
  int enviados = 0;

  for (int i = 0; i < numEventosOff; i++) {
    String payload = "{\"puerta\":\"" + String(NOMBRES[eventosOff[i].puerta]) +
                     "\",\"uid\":\"" + String(eventosOff[i].uid) +
                     "\",\"concedido\":" + (eventosOff[i].concedido ? "true" : "false") +
                     ",\"hace_ms\":" + String(ahora - eventosOff[i].ms) + "}";
    if (!client.publish(TOPIC_OFFLINE, payload.c_str())) break;
    enviados++;
    client.loop();
  }

  if (enviados > 0) {
    for (int i = enviados; i < numEventosOff; i++) eventosOff[i - enviados] = eventosOff[i];
    numEventosOff -= enviados;
    Serial.printf("[SYNC] %d acceso(s) offline enviados al servidor.\n", enviados);
  }
}

// Enciende el LED verde de una puerta y programa su apagado después de los milisegundos indicados.
void encenderVerde(int idx, unsigned long ms) {
  digitalWrite(LED_VERDE[idx], HIGH);
  apagarVerdeEn[idx] = millis() + ms;
}

// Enciende el LED rojo de una puerta y programa su apagado después de los milisegundos indicados.
void encenderRojo(int idx, unsigned long ms) {
  digitalWrite(LED_ROJO[idx], HIGH);
  apagarRojoEn[idx] = millis() + ms;
}

// Bloquea un lector por lecturas repetidas: enciende los dos LEDs hasta que pase el tiempo de bloqueo.
void bloquearLector(int idx) {
  apagarVerdeEn[idx] = 0;
  apagarRojoEn[idx] = 0;
  pendienteDesde[idx] = 0;
  digitalWrite(LED_VERDE[idx], HIGH);
  digitalWrite(LED_ROJO[idx], HIGH);
  bloqueadoHasta[idx] = millis() + BLOQUEO_REPETICION_MS;
  bloqueado[idx] = true;
  repeticiones[idx] = 0;
  ultimoUid[idx] = "";
  Serial.printf("[BLOQUEO] Puerta %s bloqueada %d s por %d lecturas seguidas de la misma tarjeta.\n",
                NOMBRES[idx], BLOQUEO_REPETICION_MS / 1000, MAX_REPETICIONES);
}

// Cuenta lecturas seguidas de la misma tarjeta en un lector; devuelve true cuando se alcanza el máximo.
bool registrarRepeticion(int idx, const String &uid) {
  unsigned long ahora = millis();
  if (repeticiones[idx] > 0 && uid == ultimoUid[idx] &&
      (ahora - ultimaRepeticionMs[idx]) <= VENTANA_REPETICION_MS) {
    repeticiones[idx]++;
  } else {
    ultimoUid[idx] = uid;
    repeticiones[idx] = 1;
  }
  ultimaRepeticionMs[idx] = ahora;
  return repeticiones[idx] >= MAX_REPETICIONES;
}

// Desbloquea los lectores cuyo bloqueo terminó y apaga los LEDs cuyo tiempo de encendido ya terminó.
void actualizarLeds() {
  unsigned long ahora = millis();
  for (int i = 0; i < NUM_LECTORES; i++) {
    if (bloqueado[i]) {
      if ((long)(ahora - bloqueadoHasta[i]) >= 0) {
        bloqueado[i] = false;
        digitalWrite(LED_VERDE[i], LOW);
        digitalWrite(LED_ROJO[i], LOW);
        Serial.printf("[BLOQUEO] Puerta %s desbloqueada.\n", NOMBRES[i]);
      }
      continue;  // mientras está bloqueado, los dos LEDs se quedan encendidos
    }
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

// Devuelve el índice de una puerta a partir de su nombre, o -1 si no existe.
int buscarPuerta(const String &nombre) {
  for (int i = 0; i < NUM_LECTORES; i++) {
    if (nombre.equalsIgnoreCase(NOMBRES[i])) return i;
  }
  return -1;
}

// Atiende los mensajes MQTT: la lista de maestras o la respuesta del servidor a una tarjeta leída.
void callback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_MAESTRAS) == 0) {
    procesarMaestras(payload, length);
    return;
  }

  String mensaje = "";
  for (unsigned int i = 0; i < length; i++) mensaje += (char)payload[i];
  mensaje.trim();

  Serial.print("[MQTT] Respuesta de Node-RED: ");
  Serial.println(mensaje);

  String estado = mensaje;
  int idx = -1;

  int sep = mensaje.indexOf(':');
  if (sep > 0) {
    estado = mensaje.substring(0, sep);
    idx = buscarPuerta(mensaje.substring(sep + 1));
  }

  // Si la respuesta no trae puerta, se asigna a la única puerta que esté esperando.
  if (idx < 0) {
    int pendiente = -1, cantidad = 0;
    for (int i = 0; i < NUM_LECTORES; i++) {
      if (pendienteDesde[i]) { pendiente = i; cantidad++; }
    }
    if (cantidad == 1) idx = pendiente;
    else if (cantidad > 1 && pendienteDesde[ultimaPuerta]) idx = ultimaPuerta;
  }

  // Respuesta sin una lectura esperándola (tardía, repetida o de otra puerta): se descarta.
  if (idx < 0 || !pendienteDesde[idx]) {
    Serial.println("[MQTT] Respuesta ignorada: ninguna puerta la estaba esperando.");
    return;
  }

  Serial.printf("[LATENCIA] Puerta %s: el servidor respondió en %lu ms.\n",
                NOMBRES[idx], millis() - pendienteDesde[idx]);
  pendienteDesde[idx] = 0;  // libera el lector

  if (estado.startsWith("permitido")) {
    Serial.printf("[ACCESO] Concedido en puerta %s. Abriendo...\n", NOMBRES[idx]);
    encenderVerde(idx, 3000);
  } else {
    Serial.printf("[ACCESO] Denegado en puerta %s.\n", NOMBRES[idx]);
    encenderRojo(idx, 3000);
  }
}

// Revisa los lectores uno por uno (con una sola antena encendida) y devuelve el UID y la puerta de la tarjeta leída.
// Se saltan los lectores bloqueados, esperando respuesta del servidor o con el LED de respuesta encendido.
bool leerTarjeta(int &idx, String &uid) {
  for (int i = 0; i < NUM_LECTORES; i++) {
    if (bloqueado[i]) continue;
    if (pendienteDesde[i]) continue;
    if (apagarVerdeEn[i] || apagarRojoEn[i]) continue;  // LED de respuesta aún encendido: no leer
    if (millis() - ultimaLectura[i] < 2000) continue;

    lectores[i].PCD_AntennaOn();
    lectores[i].PCD_SetAntennaGain(codigoGanancia(gananciaDb));
    delay(8);
    bool hayTarjeta = lectores[i].PICC_IsNewCardPresent() && lectores[i].PICC_ReadCardSerial();

    if (hayTarjeta) {
      ultimaLectura[i] = millis();

      String leido = "";
      for (byte b = 0; b < lectores[i].uid.size; b++) {
        if (lectores[i].uid.uidByte[b] < 0x10) leido += "0";
        leido += String(lectores[i].uid.uidByte[b], HEX);
      }
      leido.toUpperCase();

      lectores[i].PICC_HaltA();
      lectores[i].PCD_StopCrypto1();
      lectores[i].PCD_AntennaOff();

      Serial.printf("[RFID] Puerta %s - Tarjeta: %s\n", NOMBRES[i], leido.c_str());

      // Tercera lectura seguida de la misma tarjeta: se bloquea el lector y no se procesa.
      if (registrarRepeticion(i, leido)) {
        bloquearLector(i);
        continue;
      }

      uid = leido;
      idx = i;
      ultimaPuerta = i;
      return true;
    }

    lectores[i].PCD_AntennaOff();
  }
  return false;
}

// Pasa a modo OFFLINE: cierra MQTT, despierta la tarea offline y suspende la tarea online.
void pasarAOffline(const char* motivo) {
  client.disconnect();
  modoOnline = false;
  for (int i = 0; i < NUM_LECTORES; i++) pendienteDesde[i] = 0;
  Serial.printf("[DESCONEXION] %s. >>> MODO OFFLINE (solo tarjetas maestras) <<<\n", motivo);
  vTaskResume(hTareaOffline);
  vTaskSuspend(NULL);
}

// Pasa a modo ONLINE: despierta la tarea online y suspende la tarea offline.
void pasarAOnline() {
  modoOnline = true;
  Serial.println("[CONEXION] MQTT conectado. >>> MODO ONLINE (valida el servidor) <<<");
  vTaskResume(hTareaOnline);
  vTaskSuspend(NULL);
}

// Prueba el servidor MQTT configurado, abre la sesión y se suscribe a los temas; devuelve true si quedó conectado.
bool conectarServidor() {
  xSemaphoreTake(mtxConfig, portMAX_DELAY);
  String host = mqttHost;
  String usuario = mqttUser;
  String clave = mqttPass;
  uint16_t puerto = mqttPuerto;
  xSemaphoreGive(mtxConfig);

  if (!espClient.connect(host.c_str(), puerto, 3000)) {
    espClient.stop();
    Serial.println("[CONEXION] Servidor inalcanzable.");
    return false;
  }
  espClient.stop();

  strlcpy(hostActual, host.c_str(), sizeof(hostActual));
  client.setServer(hostActual, puerto);

  bool ok = usuario.length() ? client.connect(idCliente.c_str(), usuario.c_str(), clave.c_str())
                             : client.connect(idCliente.c_str());
  if (!ok) {
    Serial.printf("[CONEXION] MQTT fallo, rc=%d\n", client.state());
    return false;
  }

  client.subscribe(TOPIC_RESPUESTA);
  client.subscribe(TOPIC_MAESTRAS, 1);
  return true;
}

// Tarea del modo online: mantiene la conexión, sincroniza los accesos offline y manda cada tarjeta al servidor para validarla.
void TareaOnline(void *pvParameters) {
  vTaskSuspend(NULL);

  for (;;) {
    if (reintentar) {
      pasarAOffline("Cambió la configuración");
    } else if (WiFi.status() != WL_CONNECTED) {
      pasarAOffline("Se perdió el WiFi");
    } else if (!client.connected()) {
      pasarAOffline("Se perdió la conexión MQTT");
    }

    client.loop();

    if (numEventosOff > 0) enviarEventosOffline();

    for (int i = 0; i < NUM_LECTORES; i++) {
      if (pendienteDesde[i] && millis() - pendienteDesde[i] > TIMEOUT_RESPUESTA_MS) {
        pendienteDesde[i] = 0;  // libera el lector
        Serial.printf("[TIMEOUT] Sin respuesta del servidor en puerta %s. Acceso denegado.\n", NOMBRES[i]);
        encenderRojo(i, 3000);
      }
    }

    int idx;
    String uid;
    if (leerTarjeta(idx, uid)) {
      String payload = "{\"puerta\":\"" + String(NOMBRES[idx]) +
                       "\", \"uid\":\"" + uid + "\"}";

      // El lector queda bloqueado desde antes de publicar, hasta que llegue la respuesta o el timeout.
      pendienteDesde[idx] = millis();
      if (client.publish(TOPIC_RFID, payload.c_str())) {
        Serial.printf("[MQTT] Puerta %s: datos enviados para validación.\n", NOMBRES[idx]);
      } else {
        pendienteDesde[idx] = 0;
        Serial.printf("[MQTT] ERROR: no se pudo enviar la tarjeta de la puerta %s.\n", NOMBRES[idx]);
        encenderRojo(idx, 2000);
      }
    }

    actualizarLeds();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Tarea del modo offline: acepta solo las tarjetas maestras guardadas, registra cada acceso e intenta reconectar cada 10 segundos.
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

    bool forzar = reintentar;
    if (forzar || millis() - ultimoIntento > 10000) {
      ultimoIntento = millis();
      reintentar = false;

      if (!wifiConfigurado()) {
        Serial.println("[CONFIG] Falta el WiFi. Escribe: ssid NOMBRE  y  pass CLAVE");
      } else if (wifiNuevo || WiFi.status() != WL_CONNECTED) {
        if (wifiNuevo) {
          wifiNuevo = false;
          WiFi.disconnect();
        }
        Serial.println("[CONEXION] Conectando a WiFi...");
        iniciarWifi();
      } else if (!servidorConfigurado()) {
        Serial.println("[CONFIG] Falta el servidor. Escribe: server DIRECCION  (y mqttuser / mqttpass si los necesita)");
      } else if (conectarServidor()) {
        pasarAOnline();
      }
    }

    actualizarLeds();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Carga la configuración, inicia los lectores RFID y crea las tareas online y offline arrancando en modo offline.
void setup() {
  Serial.begin(115200);

  mtxConfig = xSemaphoreCreateMutex();
  idCliente = "ESP32_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  cargarConfig();
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

  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, LOW);
  delay(10);
  digitalWrite(RST_PIN, HIGH);
  delay(50);

  for (int i = 0; i < NUM_LECTORES; i++) {
    lectores[i].PCD_Init();
    byte v = lectores[i].PCD_ReadRegister(MFRC522::VersionReg);
    lectores[i].PCD_SetAntennaGain(codigoGanancia(gananciaDb));
    lectores[i].PCD_AntennaOff();
    Serial.printf("[SISTEMA] Lector %s iniciado (version 0x%02X).\n", NOMBRES[i], v);
  }

  client.setCallback(callback);
  client.setKeepAlive(15);
  client.setSocketTimeout(3);
  client.setBufferSize(512);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  Serial.println("[SISTEMA] Iniciando en modo OFFLINE hasta conectar con el servidor.");
  Serial.println("[SISTEMA] Escribe 'ayuda' para ver los comandos de configuración.");

  xTaskCreatePinnedToCore(TareaOffline, "TareaOffline", 6144, NULL, 1, &hTareaOffline, 1);
  xTaskCreatePinnedToCore(TareaOnline,  "TareaOnline",  8192, NULL, 1, &hTareaOnline,  1);
}

String lineaSerial = "";

// Lee los comandos escritos en el monitor serie; el trabajo de acceso lo hacen las tareas de FreeRTOS.
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
