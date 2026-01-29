#ifndef SYSTEM_CONFIG_H
#define SYSTEM_CONFIG_H

// =============================================================================
// INCLUDES BÁSICAS DEL SISTEMA
// =============================================================================
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include <math.h>

// =============================================================================
// INCLUDES ESP-IDF FREERTOS
// =============================================================================
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

// =============================================================================
// INCLUDES ESP-IDF CORE
// =============================================================================
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"

// =============================================================================
// INCLUDES ESP-IDF PERIFÉRICOS
// =============================================================================
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "driver/i2c.h"
#include "driver/adc.h"

// =============================================================================
// INCLUDES DE COMUNICACIÓN
// =============================================================================
#include "mqtt_client.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_http_server.h"

// =============================================================================
// CONFIGURACIÓN RED Y COMUNICACIONES
// =============================================================================

// WiFi - Credenciales de red
#define WIFI_SSID                 "SBC"           // SSID de la red WiFi
#define WIFI_PASS                 "SBCwifi$"      // Contraseña WiFi
#define MAX_INTENTOS              3               // Intentos de conexión WiFi
#define AP_PASSWORD               "config123"     // Contraseña del Access Point

// MQTT - ThingsBoard
#define THINGSBOARD_MQTT_URI      "mqtt://demo.thingsboard.io"           // URI del broker MQTT
#define THINGSBOARD_ACCESS_TOKEN  "om8tccnqiv43ukzaprgp"                 // Token de acceso ThingsBoard

// OTA - Actualizaciones de firmware
#define GITHUB_FIRMWARE_URL       "https://raw.githubusercontent.com/carlxx2/Estacion_Meteo_T07/main/firmware/firmware.bin"

// =============================================================================
// CONFIGURACIÓN HARDWARE - ADC (SENSORES ANALÓGICOS)
// =============================================================================

// Canales ADC
#define PLUVIOMETRO_ADC_CHANNEL   ADC1_CHANNEL_4   // GPIO32 - Pluviómetro
#define ANEMOMETRO_ADC_CHANNEL    ADC1_CHANNEL_5   // GPIO33 - Anemómetro

// Calibración Pluviómetro (voltaje → mm de lluvia)
#define PLUVIOMETRO_MIN_VOLTAGE   0.1f             // Voltaje mínimo (0 mm)
#define PLUVIOMETRO_MAX_VOLTAGE   2.5f             // Voltaje máximo
#define PLUVIOMETRO_MAX_MM        50.0f            // Lluvia máxima en mm

// Calibración Anemómetro (voltaje → m/s)
#define ANEMOMETRO_MIN_VOLTAGE    0.5f             // Voltaje mínimo (0 m/s)
#define ANEMOMETRO_MAX_VOLTAGE    2.8f             // Voltaje máximo
#define ANEMOMETRO_MAX_MS         20.0f            // Velocidad máxima en m/s

// =============================================================================
// CONFIGURACIÓN HARDWARE - BME680 (SENSOR I2C)
// =============================================================================

// Configuración I2C
#define I2C_MASTER_NUM            I2C_NUM_0        // Puerto I2C 0
#define I2C_MASTER_SDA_IO         21               // GPIO21 - SDA
#define I2C_MASTER_SCL_IO         22               // GPIO22 - SCL
#define I2C_MASTER_FREQ_HZ        100000           // Frecuencia 100kHz
#define BME680_ADDR               0x77             // Dirección I2C del BME680

// Calibración BME680 (ajustes de escala y offset)
#define BME680_TEMP_SCALE         1.0f             // Factor escala temperatura
#define BME680_TEMP_OFFSET_C      0.0f             // Offset temperatura (°C)
#define BME680_HUM_SCALE          1.4f             // Factor escala humedad
#define BME680_HUM_OFFSET_PCT     0.0f             // Offset humedad (%)

// =============================================================================
// CONFIGURACIÓN SISTEMA - BUFFER DE DATOS
// =============================================================================

#define MAX_BUFFER_SIZE           100              // Capacidad máxima del buffer circular

// =============================================================================
// CONFIGURACIÓN SISTEMA DE TIEMPO (NTP)
// =============================================================================

#define NTP_SERVER1             "pool.ntp.org"           // Servidor NTP primario
#define NTP_SERVER2             "time.google.com"        // Servidor NTP secundario  
#define NTP_SERVER3             "time.windows.com"       // Servidor NTP terciario
#define TIME_ZONE               "CET-1CEST-2,M3.5.0/02:00:00,M10.5.0/03:00:00"  // Zona horaria España
#define NTP_SYNC_TIMEOUT_MS     10000                    // Timeout sincronización NTP (10s)

// =============================================================================
// TIPOS DE DATOS - ENUMERACIONES
// =============================================================================

/**
 * @brief Modos de operación del sensor BME680
 * 
 * SLEEP_MODE:    Bajo consumo, solo responde a I2C
 * FORCED_MODE:   Una medición, luego vuelve a sleep
 * PARALLEL_MODE: Múltiples mediciones paralelas (gas)
 * NORMAL_MODE:   Mediciones continuas automáticas
 */
typedef enum {
    BME680_SLEEP_MODE = 0,     // Modo bajo consumo
    BME680_FORCED_MODE,        // Mediciones bajo demanda
    BME680_PARALLEL_MODE,      // Mediciones paralelas de gas
    BME680_NORMAL_MODE = 3     // Mediciones continuas
} bme680_mode_t;

/**
 * @brief Tasas de oversampling para mayor precisión
 * 
 * Controla cuántas muestras toma el sensor internamente
 * Mayor oversampling = mayor precisión = mayor consumo
 */
typedef enum {
    BME680_OSR_NONE = 0,       // Sin oversampling
    BME680_OSR_1X,             // Oversampling 1x
    BME680_OSR_2X,             // Oversampling 2x
    BME680_OSR_4X,             // Oversampling 4x
    BME680_OSR_8X,             // Oversampling 8x
    BME680_OSR_16X             // Oversampling 16x (máxima precisión)
} bme680_osr_t;

// =============================================================================
// ESTRUCTURAS DE DATOS - SENSORES
// =============================================================================

/**
 * @brief Datos crudos del sensor BME680
 * 
 * Contiene todas las mediciones del sensor más datos crudos
 * para diagnóstico. Usar cuando se necesite acceso a raw_gas.
 */
typedef struct {
    float temperature;          // Temperatura en °C
    float humidity;             // Humedad relativa en %
    float pressure;             // Presión atmosférica en hPa
    uint32_t gas_resistance;    // Resistencia del sensor de gas en Ω
    float air_quality;          // Calidad del aire calculada (0-100%)
    int raw_gas;                // Valor ADC crudo del sensor de gas (solo debug)
} bme680_data_t;

/**
 * @brief Configuración del sensor BME680
 * 
 * Parámetros para configurar el modo de operación,
 * oversampling y calentador del sensor.
 */
typedef struct {
    bme680_osr_t osr_temperature;   // Oversampling temperatura
    bme680_osr_t osr_pressure;      // Oversampling presión
    bme680_osr_t osr_humidity;      // Oversampling humedad
    bme680_mode_t operation_mode;   // Modo de operación
    uint16_t heater_temperature;    // Temperatura del calentador (°C)
    uint16_t heater_duration;       // Duración del calentador (ms)
    int16_t ambient_temperature;    // Temperatura ambiente estimada (°C)
} bme680_config_t;

// =============================================================================
// ESTRUCTURAS DE DATOS - ALMACENAMIENTO
// =============================================================================

/**
 * @brief Lectura completa para almacenamiento/envió
 * 
 * Combina datos del BME680 con sensores meteorológicos.
 * Usar para almacenar en buffer local o enviar por MQTT.
 * NO incluye timestamps ni datos crudos de debug.
 */
typedef struct {
    float temperature;          // Temperatura en °C
    float humidity;             // Humedad relativa en %
    float pressure;             // Presión atmosférica en hPa
    uint32_t gas_resistance;    // Resistencia del sensor de gas en Ω
    float air_quality;          // Calidad del aire calculada (0-100%)
    float rainfall_mm;          // Lluvia acumulada en mm
    float wind_speed_ms;        // Velocidad del viento en m/s
    // Timestamp y metadata
    time_t timestamp;           // Segundos desde epoch Unix (1 Jan 1970)
    bool time_synced;           // true = sincronizado con NTP, false = estimado
} stored_reading_t;

/**
 * @brief Buffer circular para almacenamiento local
 * 
 * Implementa un buffer FIFO de tamaño fijo para almacenar
 * lecturas cuando no hay conexión a Internet.
 */
typedef struct {
    stored_reading_t readings[MAX_BUFFER_SIZE];  // Array de lecturas
    uint16_t head;                               // Índice de escritura
    uint16_t tail;                               // Índice de lectura
    uint16_t count;                              // Número de lecturas almacenadas
    bool buffer_full;                            // Flag de buffer lleno
} circular_buffer_t;

// =============================================================================
// DECLARACIONES DE FUNCIONES - POR MÓDULO
// =============================================================================

// -----------------------------------------------------------------------------
// MÓDULO: WiFi Manager (wifi_manager.c)
// -----------------------------------------------------------------------------
void wifi_init_sta(void);                       // Inicializa conexión WiFi
bool wifi_is_connected(void);                   // Verifica conexión WiFi activa
bool wifi_is_ap_mode(void);                     // Verifica si está en modo AP
const char* wifi_get_ap_ssid(void);             // Obtiene SSID del AP

// -----------------------------------------------------------------------------
// MÓDULO: MQTT Client (mqtt_client.c)
// -----------------------------------------------------------------------------
void mqtt_init(void);                           // Inicializa cliente MQTT
void send_mqtt_telemetry(bme680_data_t *bme_data, float rainfall_mm, float wind_speed_ms);  // Envía telemetría
bool send_mqtt_telemetry_with_timestamp(const char *json_message);  // Envía JSON con timestamp
bool mqtt_is_connected(void);                   // Verifica conexión MQTT
void mqtt_force_reconnect(void);                  // Fuerza reconexión completa
void mqtt_check_and_reconnect(void);              // Verifica y reconecta periódicamente

// -----------------------------------------------------------------------------
// MÓDULO: OTA Updater (ota_updater.c)
// -----------------------------------------------------------------------------
void check_ota_updates(void);                   // Verifica y aplica actualizaciones OTA

// -----------------------------------------------------------------------------
// MÓDULO: Sensor Reader (sensor_reader.c)
// -----------------------------------------------------------------------------
void init_sensors(void);                        // Inicializa sensores ADC
float read_pluviometro_value(void);             // Lee valor del pluviómetro (mm)
float read_anemometro_value(void);              // Lee valor del anemómetro (m/s)

// -----------------------------------------------------------------------------
// MÓDULO: BME680 Sensor (bme680_sensor.c)
// -----------------------------------------------------------------------------
void bme680_init(void);                         // Inicializa I2C y sensor
esp_err_t bme680_read_all_data(bme680_data_t *sensor_data);  // Lee todos los datos del BME680
bool bme680_is_connected(void);                 // Verifica conexión del sensor
void bme680_start_reading_task(void);           // Inicia tarea de lectura continua
esp_err_t bme680_configure_sensor(void);        // Configura parámetros del sensor
void i2c_diagnostic(void);                      // Diagnóstico del bus I2C

// Funciones de configuración avanzada (inspiradas en gschorcht)
esp_err_t bme680_set_oversampling_rates(bme680_osr_t osr_t, bme680_osr_t osr_p, bme680_osr_t osr_h);
esp_err_t bme680_set_heater_profile(uint16_t temperature, uint16_t duration);
esp_err_t bme680_set_operation_mode(bme680_mode_t mode);
esp_err_t bme680_set_ambient_temperature(int16_t temperature);
bme680_mode_t bme680_get_operation_mode(void);
esp_err_t bme680_apply_config(bme680_config_t *config);

// -----------------------------------------------------------------------------
// MÓDULO: Data Buffer (data_buffer.c)
// -----------------------------------------------------------------------------
void data_buffer_init(void);                    // Inicializa buffer circular
bool data_buffer_store_reading(bme680_data_t *bme_data, float rainfall_mm, float wind_speed_ms);  // Almacena lectura
bool data_buffer_send_stored_readings(void);    // Envía lecturas almacenadas
uint16_t data_buffer_get_count(void);           // Obtiene número de lecturas almacenadas
bool data_buffer_is_full(void);                 // Verifica si buffer está lleno
void data_buffer_clear(void);                   // Limpia buffer completamente
void data_buffer_print_status(void);            // Muestra estado del buffer
int data_buffer_repair_corrupt_entries(void);
void data_buffer_clean_corrupt_readings_aggressive(void);

// -----------------------------------------------------------------------------
// MÓDULO: Web Server (web_server.c)
// -----------------------------------------------------------------------------
void web_server_start(void);                    // Inicia servidor web
void web_server_stop(void);                     // Detiene servidor web
bool web_server_is_running(void);               // Verifica si servidor está activo
const char* web_server_get_ip(void);            // Obtiene IP del servidor web

// -----------------------------------------------------------------------------
// MÓDULO: Sistema de Tiempo (time_manager.c)
// -----------------------------------------------------------------------------
// Inicialización y sincronización
void time_init(void);                               // Inicializa sistema de tiempo
bool time_sync_with_ntp(void);                      // Sincroniza con NTP (bloqueante)
void time_try_sync_periodic(void);                  // Sincronización periódica no bloqueante

// Obtención de tiempo
time_t time_get_current(void);                      // Tiempo actual en segundos Unix
uint64_t time_get_current_ms(void);                 // Tiempo actual en milisegundos Unix
bool time_is_synced(void);                          // Verifica si hora está sincronizada

// Formateo de tiempo
const char* time_get_current_str(void);             // Hora completa "Wed Jan  1 12:00:00 2025"
const char* time_get_current_time_str(void);        // Solo hora "12:00:00"
const char* time_get_current_date_str(void);        // Solo fecha "2025-01-01"
bool time_is_timestamp_valid(time_t timestamp);
time_t time_get_validated_current(void);

// Configuración manual
void time_set_manual(time_t manual_time);           // Establece hora manualmente

// Diagnóstico
void time_print_status(void);                       // Muestra estado del sistema de tiempo

#endif // SYSTEM_CONFIG_H