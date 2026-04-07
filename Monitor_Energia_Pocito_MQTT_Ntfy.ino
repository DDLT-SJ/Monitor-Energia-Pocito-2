/*
 * Monitor de Energía Pocito
 * ESP32-C3 + PCF8574 + MQTT (HiveMQ) + Ntfy
 *
 * Hardware:
 * - Waveshare ESP32-C3 Zero
 * - PCF8574 I/O Expander (I2C: GPIO6=SDA, GPIO7=SCL)
 * - 8x PC817 (LOW = falla, HIGH = normal, con pull-up)
 *
 * Flujo:
 * - Detecta cambios en entradas
 * - Envía notificación Ntfy push
 * - Publica estado en MQTT para dashboard
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <Wire.h>

// ============================================================================
// CONFIGURACIÓN MQTT - HiveMQ Cloud
// ============================================================================
#define MQTT_SERVER "748863d67a8940b8957fbc4523ac7f5d.s1.eu.hivemq.cloud"
#define MQTT_PORT 8883
#define MQTT_USER "Daniel_SJ"
#define MQTT_PASS "Ddelatorre1966"
#define MQTT_TOPIC "Energia/Pocito"
#define MQTT_CLIENT_ID "ESP32C3_Pocito"

// ============================================================================
// CONFIGURACIÓN NTfy
// ============================================================================
#define NTfy_TOPIC "Monitor_Energia_Pocito"
#define NTfy_SERVER "https://ntfy.sh"

// ============================================================================
// NOMBRES DE LAS ENTRADAS
// ============================================================================
const char* PIN_NOMBRES[8] = {
  "Entrada termomagnética Principal",
  "Salida Termomagnética Principal",
  "Salida Protección Sobrevoltaje",
  "Salida Disyuntor Diferencial",
  "Salida Termomagnética Tomas Comedor (Heladera)",
  "Salida Termomagnética Iluminación Comedor (Cámaras)",
  "Salida Termomagnética Bombas Pozo y Riego",
  "Salida Termomagnética Bomba Piscina"
};

// ============================================================================
// VARIABLES GLOBALES
// ============================================================================
WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

uint8_t estadoActual = 0xFF;
uint8_t estadoAnterior = 0xFF;
unsigned long ultimoDebounce = 0;
const unsigned long debounceDelay = 100;

unsigned long ultimoIntentoMQTT = 0;
const unsigned long intervaloReconexion = 5000;

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);

  // Inicializar I2C (GPIO6=SDA, GPIO7=SCL en ESP32-C3)
  Wire.begin(6, 7);

  // Verificar PCF8574
  Wire.beginTransmission(0x20);
  if (Wire.endTransmission() != 0) {
    Serial.println("ERROR: PCF8574 no detectado!");
    while (1) {
      delay(1000);
      Serial.println("Revisar conexiones I2C...");
    }
  }
  Serial.println("PCF8574 detectado OK");

  // Configurar WiFiManager
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  if (!wm.autoConnect("ESP32_Pocito_Config", "config123")) {
    Serial.println("Fallo conexion AP - reiniciando...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("WiFi conectado OK");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // Configurar MQTT con TLS
  wifiClient.setInsecure();  // TLS sin verificar certificado (más simple)
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);

  Serial.println("\n=== Monitor Energía Pocito Iniciado ===");
}

// ============================================================================
// LOOP
// ============================================================================
void loop() {
  // Mantener conexión MQTT
  if (!mqttClient.connected()) {
    conectarMQTT();
  }
  mqttClient.loop();

  // Leer PCF8574
  Wire.requestFrom(0x20, 1);
  if (Wire.available()) {
    estadoActual = Wire.read();
  }

  // Detectar cambios con debounce
  if (estadoActual != estadoAnterior) {
    ultimoDebounce = millis();
  }

  if ((millis() - ultimoDebounce) > debounceDelay) {
    if (estadoActual != estadoAnterior) {
      procesarCambios(estadoAnterior, estadoActual);
      estadoAnterior = estadoActual;
      // Publicar estado completo en MQTT
      publicarEstadoCompleto();
    }
  }

  delay(100);
}

// ============================================================================
// PROCESAR CAMBIOS DE ESTADO
// ============================================================================
void procesarCambios(uint8_t anterior, uint8_t actual) {
  for (int i = 0; i < 8; i++) {
    bool estadoPrev = bitRead(anterior, i);
    bool estadoAct = bitRead(actual, i);

    if (estadoPrev != estadoAct) {
      bool esFalla = !estadoAct;  // LOW = falla
      enviarNotificacion(i, esFalla);

      Serial.print(esFalla ? "FALLA: " : "REPOSICION: ");
      Serial.println(PIN_NOMBRES[i]);
    }
  }
}

// ============================================================================
// ENVIAR NOTIFICACIÓN A NTfy
// ============================================================================
void enviarNotificacion(int pin, bool esFalla) {
  String titulo = esFalla ? "Falla de Energía Pocito" : "Reposición Energía Pocito";
  String mensaje = esFalla ? "Corte de energía en " : "Energía restablecida en ";
  mensaje += PIN_NOMBRES[pin];
  String emoji = esFalla ? "🔴" : "🟢";

  HTTPClient http;
  http.begin(String(NTfy_SERVER) + "/" + NTfy_TOPIC);
  http.addHeader("Title", titulo);
  http.addHeader("Priority", "3");
  http.addHeader("Tags", emoji);
  http.addHeader("Content-Type", "text/plain");

  int httpCode = http.POST(mensaje);
  Serial.print("Ntfy HTTP: ");
  Serial.println(httpCode);

  http.end();
}

// ============================================================================
// PUBLICAR ESTADO COMPLETO EN MQTT (para dashboard)
// ============================================================================
void publicarEstadoCompleto() {
  // Formato JSON con estado de cada entrada
  String json = "{";
  json += "\"timestamp\":" + String(millis());
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"entradas\":{";

  for (int i = 0; i < 8; i++) {
    bool estado = bitRead(estadoActual, i);
    json += "\"P" + String(i) + "\":" + String(estado ? "1" : "0");
    if (i < 7) json += ",";
  }

  json += "}}";

  mqttClient.publish(MQTT_TOPIC, json.c_str());

  Serial.print("MQTT publicado: ");
  Serial.println(json);
}

// ============================================================================
// CONECTAR MQTT
// ============================================================================
void conectarMQTT() {
  if (millis() - ultimoIntentoMQTT < intervaloReconexion) return;

  Serial.print("Conectando MQTT...");

  if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
    Serial.println("OK");
    ultimoIntentoMQTT = 0;

    // Publicar estado de conexión
    String json = "{\"estado\":\"conectado\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
    mqttClient.publish(MQTT_TOPIC, json.c_str());
  } else {
    Serial.print("ERROR (");
    Serial.print(mqttClient.state());
    Serial.println(")");
    ultimoIntentoMQTT = millis();
  }
}
