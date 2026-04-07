/*
 * Monitor de Energía Pocito - Versión Simple
 * ESP32-C3 + PCF8574 + Ntfy (HTTP directo)
 *
 * Hardware:
 * - Waveshare ESP32-C3 Zero
 * - PCF8574 I/O Expander (I2C: GPIO6=SDA, GPIO7=SCL)
 * - 8x PC817 (LOW = falla, HIGH = normal, con pull-up)
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <Wire.h>

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
uint8_t estadoActual = 0xFF;
uint8_t estadoAnterior = 0xFF;
unsigned long ultimoDebounce = 0;
const unsigned long debounceDelay = 100;

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
  Serial.println("\n=== Monitor Energía Pocito Iniciado ===");
}

// ============================================================================
// LOOP
// ============================================================================
void loop() {
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
