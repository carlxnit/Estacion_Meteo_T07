#include "system_config.h"
#include <inttypes.h>

static const char *TAG = "DATA_BUFFER";

// =============================================================================
// VARIABLES PRIVADAS
// =============================================================================
static circular_buffer_t sensor_buffer = {0};
static bool buffer_initialized = false;
static time_t boot_time_reference = 0;  // Referencia de tiempo de boot

// =============================================================================
// FUNCIONES PRIVADAS (UTILIDAD)
// =============================================================================

static bool save_buffer_to_nvs(void) {
    if (sensor_buffer.count == 0) {
        return true;
    }
    
    nvs_handle_t handle;
    esp_err_t err;
    
    err = nvs_open("sensor_storage", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS: %s", esp_err_to_name(err));
        return false;
    }
    
    err = nvs_set_u16(handle, "head", sensor_buffer.head);
    err |= nvs_set_u16(handle, "tail", sensor_buffer.tail);
    err |= nvs_set_u16(handle, "count", sensor_buffer.count);
    err |= nvs_set_u8(handle, "full", sensor_buffer.buffer_full ? 1 : 0);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error guardando metadata");
        nvs_close(handle);
        return false;
    }
    
    for (uint16_t i = 0; i < sensor_buffer.count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "rd%04d", i);
        
        uint16_t idx = (sensor_buffer.tail + i) % MAX_BUFFER_SIZE;
        err = nvs_set_blob(handle, key, &sensor_buffer.readings[idx], sizeof(stored_reading_t));
        
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error guardando lectura %d: %s", i, esp_err_to_name(err));
            break;
        }
    }
    
    err = nvs_commit(handle);
    nvs_close(handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error en commit NVS: %s", esp_err_to_name(err));
        return false;
    }
    
    ESP_LOGD(TAG, "Guardado en NVS: %d lecturas", sensor_buffer.count);
    return true;
}

static bool load_buffer_from_nvs(void) {
    nvs_handle_t handle;
    esp_err_t err;
    
    err = nvs_open("sensor_storage", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No hay datos previos en NVS");
        return false;
    }
    
    uint16_t head, tail, count;
    uint8_t full;
    
    err = nvs_get_u16(handle, "head", &head);
    err |= nvs_get_u16(handle, "tail", &tail);
    err |= nvs_get_u16(handle, "count", &count);
    err |= nvs_get_u8(handle, "full", &full);
    
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error cargando metadata: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }
    
    if (count > MAX_BUFFER_SIZE) {
        ESP_LOGW(TAG, "Datos corruptos: count=%d > MAX=%d", count, MAX_BUFFER_SIZE);
        nvs_close(handle);
        return false;
    }
    
    sensor_buffer.head = head;
    sensor_buffer.tail = tail;
    sensor_buffer.count = count;
    sensor_buffer.buffer_full = (full == 1);
    
    for (uint16_t i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "rd%03d", i);
        
        size_t required_size = sizeof(stored_reading_t);
        uint16_t idx = (tail + i) % MAX_BUFFER_SIZE;
        
        err = nvs_get_blob(handle, key, &sensor_buffer.readings[idx], &required_size);
        if (err != ESP_OK || required_size != sizeof(stored_reading_t)) {
            ESP_LOGE(TAG, "Error cargando lectura %d", i);
            nvs_close(handle);
            return false;
        }
    }
    
    nvs_close(handle);
    ESP_LOGI(TAG, "Cargado desde NVS: %d lecturas", sensor_buffer.count);
    return true;
}

/**
 * @brief Validar que una lectura es valida para almacenar
 */
static bool is_sensor_data_valid(bme680_data_t *bme_data, float rainfall_mm, float wind_speed_ms) {
    // Validar datos BME680
    if (bme_data->temperature < -50.0 || bme_data->temperature > 100.0) {
        ESP_LOGW(TAG, "Temperatura invalida: %.2fC", bme_data->temperature);
        return false;
    }
    
    if (bme_data->humidity < 0.0 || bme_data->humidity > 100.0) {
        ESP_LOGW(TAG, "Humedad invalida: %.2f%%", bme_data->humidity);
        return false;
    }
    
    if (bme_data->pressure < 300.0 || bme_data->pressure > 1100.0) {
        ESP_LOGW(TAG, "Presion invalida: %.2fhPa", bme_data->pressure);
        return false;
    }
    
    if (bme_data->air_quality < 0.0 || bme_data->air_quality > 500.0) {
        ESP_LOGW(TAG, "Calidad aire invalida: %.2f", bme_data->air_quality);
        return false;
    }
    
    // Validar sensores meteorologicos
    if (rainfall_mm < 0.0 || rainfall_mm > 1000.0) {
        ESP_LOGW(TAG, "Lluvia invalida: %.2fmm", rainfall_mm);
        return false;
    }
    
    if (wind_speed_ms < 0.0 || wind_speed_ms > 100.0) {
        ESP_LOGW(TAG, "Viento invalido: %.2fm/s", wind_speed_ms);
        return false;
    }
    
    return true;
}

/**
 * @brief Asigna timestamp estimado a lecturas sin timestamp
 * 
 * Basado en:
 * 1. Tiempo actual estimado (si el sistema de tiempo esta funcionando)
 * 2. Orden de las lecturas en el buffer (las mas antiguas primero)
 * 3. Intervalo de 10 segundos entre lecturas (configuracion del sistema)
 */
static void fix_timestamps_in_buffer(void) {
    if (sensor_buffer.count == 0) {
        return;
    }
    
    ESP_LOGI(TAG, "Reparando timestamps en buffer...");
    
    // Obtener tiempo actual del SISTEMA (no estimado)
    time_t current_system_time = time(NULL);
    
    // Si el tiempo del sistema es invalido (0 o muy antiguo)
    if (current_system_time < 1577836800) {  // Anterior a 2020
        // Usar tiempo de referencia conservador (1 hora atras del tiempo de compilacion)
        // Esto evita timestamps futuros
        current_system_time = 1700000000;  // Aprox Nov 2023
        ESP_LOGW(TAG, "Tiempo del sistema invalido, usando referencia: %ld", 
                 (long)current_system_time);
    }
    
    // Verificar que el tiempo no sea futuro ridiculo
    time_t safe_current_time = current_system_time;
    
    // Si el tiempo parece estar en el futuro (mas de 24 horas adelante del tiempo real)
    time_t real_time = time(NULL);  // Tiempo real del sistema
    if (real_time > 0 && safe_current_time > (real_time + 86400)) {  // +24 horas
        ESP_LOGW(TAG, "Tiempo calculado parece futuro, ajustando...");
        safe_current_time = real_time;
    }
    
    // Calcular timestamp para la lectura mas antigua (al menos 5 minutos por lectura)
    // Esto asegura orden cronologico incluso si hubo desconexion larga
    time_t oldest_timestamp = safe_current_time - (sensor_buffer.count * 600);  // 10 min por lectura
    
    // Asegurar que el timestamp mas antiguo no sea anterior a 2020
    if (oldest_timestamp < 1577836800) {
        oldest_timestamp = 1577836800 + (sensor_buffer.count * 10);  // Minimo 2020
    }
    
    ESP_LOGI(TAG, "Base de reparacion: tiempo actual seguro=%ld, mas antiguo=%ld", 
             (long)safe_current_time, (long)oldest_timestamp);
    
    // Asignar timestamps en orden cronologico (mas antiguas primero)
    int repaired_count = 0;
    for (int i = 0; i < sensor_buffer.count; i++) {
        uint16_t idx = (sensor_buffer.tail + i) % MAX_BUFFER_SIZE;
        stored_reading_t *reading = &sensor_buffer.readings[idx];
        
        // Solo reparar si el timestamp es invalido (< 2020 o 0)
        if (reading->timestamp < 1577836800) {
            // Calcular timestamp estimado: mas antiguo + (intervalo * posicion)
            // Usamos 10 segundos entre lecturas (como el ciclo normal)
            time_t estimated_time = oldest_timestamp + (i * 10);
            
            // Validar que no sea futuro (mas de 1 hora en el futuro)
            if (estimated_time > (safe_current_time + 3600)) {
                ESP_LOGW(TAG, "Timestamp estimado %ld es futuro, ajustando", 
                         (long)estimated_time);
                estimated_time = safe_current_time - ((sensor_buffer.count - i) * 10);
            }
            
            // Validar que sea razonable (> 2020)
            if (estimated_time >= 1577836800) {
                time_t old_time = reading->timestamp;
                reading->timestamp = estimated_time;
                reading->time_synced = false;  // Marcar como tiempo estimado
                
                // Convertir a string para log
                struct tm *timeinfo = localtime(&estimated_time);
                char time_str[32];
                strftime(time_str, sizeof(time_str), "%H:%M:%S %d/%m/%Y", timeinfo);
                
                ESP_LOGI(TAG, "  Lectura %d[%d]: %ld -> %ld (%s)", 
                         i, idx, (long)old_time, (long)estimated_time, time_str);
                repaired_count++;
            } else {
                ESP_LOGW(TAG, "  Lectura %d[%d]: Timestamp estimado invalido %ld", 
                         i, idx, (long)estimated_time);
            }
        } else if (reading->timestamp > (safe_current_time + 3600)) {
            // Si el timestamp existe pero es futuro (> 1 hora), corregirlo
            ESP_LOGW(TAG, "  Lectura %d[%d]: Timestamp futuro %ld detectado", 
                     i, idx, (long)reading->timestamp);
            
            // Ajustar a tiempo actual - posicion relativa
            time_t corrected_time = safe_current_time - ((sensor_buffer.count - i) * 10);
            if (corrected_time < 1577836800) corrected_time = 1577836800 + i;
            
            reading->timestamp = corrected_time;
            reading->time_synced = false;
            repaired_count++;
        }
    }
    
    if (repaired_count > 0) {
        ESP_LOGI(TAG, "Reparados %d timestamps en buffer", repaired_count);
        
        // Guardar cambios en NVS
        if (save_buffer_to_nvs()) {
            ESP_LOGI(TAG, "Buffer reparado guardado en NVS");
        }
    } else {
        ESP_LOGI(TAG, "No se necesitaron reparaciones de timestamp");
    }
}

/**
 * @brief Limpia completamente lecturas con valores nulos/inválidos
 * Versión agresiva para limpieza total
 */
void data_buffer_clean_corrupt_readings_aggressive(void) {
    if (!buffer_initialized || sensor_buffer.count == 0) {
        return;
    }
    
    ESP_LOGI(TAG, "=== LIMPIEZA AGRESIVA DE BUFFER ===");
    
    int original_count = sensor_buffer.count;
    int valid_count = 0;
    
    // Crear buffer temporal para lecturas válidas
    stored_reading_t valid_readings[MAX_BUFFER_SIZE];
    
    // Filtrar solo lecturas válidas
    for (int i = 0; i < sensor_buffer.count; i++) {
        uint16_t idx = (sensor_buffer.tail + i) % MAX_BUFFER_SIZE;
        stored_reading_t *reading = &sensor_buffer.readings[idx];
        
        // Criterios estrictos de validez
        bool is_valid = 
            reading->temperature > -40.0f && reading->temperature < 85.0f &&
            reading->humidity >= 0.0f && reading->humidity <= 100.0f &&
            reading->pressure >= 300.0f && reading->pressure <= 1100.0f &&
            reading->rainfall_mm >= 0.0f && reading->rainfall_mm <= 1000.0f &&
            reading->wind_speed_ms >= 0.0f && reading->wind_speed_ms <= 100.0f &&
            reading->timestamp >= 1577836800; // Después de 2020
        
        if (is_valid) {
            valid_readings[valid_count] = *reading;
            valid_count++;
        } else {
            struct tm *timeinfo = localtime(&reading->timestamp);
            char time_str[32];
            strftime(time_str, sizeof(time_str), "%H:%M:%S", timeinfo);
            
            ESP_LOGW(TAG, "Eliminando lectura inválida [%d] %s:", idx, time_str);
            ESP_LOGW(TAG, "  T=%.2f, H=%.2f, P=%.2f, R=%.2f, W=%.2f",
                     reading->temperature, reading->humidity, reading->pressure,
                     reading->rainfall_mm, reading->wind_speed_ms);
        }
    }
    
    // Copiar lecturas válidas de vuelta al buffer
    for (int i = 0; i < valid_count; i++) {
        sensor_buffer.readings[i] = valid_readings[i];
    }
    
    // Actualizar índices del buffer
    sensor_buffer.head = valid_count % MAX_BUFFER_SIZE;
    sensor_buffer.tail = 0;
    sensor_buffer.count = valid_count;
    sensor_buffer.buffer_full = (valid_count >= MAX_BUFFER_SIZE);
    
    // Guardar en NVS
    if (save_buffer_to_nvs()) {
        ESP_LOGI(TAG, "Buffer limpio guardado en NVS");
    }
    
    ESP_LOGI(TAG, "Limpieza completada: %d -> %d lecturas válidas",
             original_count, valid_count);
    ESP_LOGI(TAG, "================================");
}

// =============================================================================
// FUNCIONES PUBLICAS
// =============================================================================

void data_buffer_init(void) {
    ESP_LOGI(TAG, "Inicializando buffer circular...");
    
    memset(&sensor_buffer, 0, sizeof(circular_buffer_t));
    sensor_buffer.head = 0;
    sensor_buffer.tail = 0;
    sensor_buffer.count = 0;
    sensor_buffer.buffer_full = false;
    
    // Establecer referencia de tiempo de boot
    boot_time_reference = 1735689600;  // 2025-01-01 00:00:00 UTC
    
    if (load_buffer_from_nvs()) {
        ESP_LOGI(TAG, "Datos previos cargados: %d lecturas", sensor_buffer.count);
        
        // ⭐⭐ NUEVO: Reparar lecturas corruptas automáticamente ⭐⭐
        if (sensor_buffer.count > 0) {
            ESP_LOGI(TAG, "Ejecutando reparación automática de buffer...");
            
            // Primero reparación normal
            int repaired = data_buffer_repair_corrupt_entries();
            
            // Si aún hay muchos problemas, limpieza agresiva
            if (sensor_buffer.count > 0 && repaired > sensor_buffer.count / 2) {
                ESP_LOGW(TAG, "Muchas lecturas corruptas, aplicando limpieza agresiva...");
                data_buffer_clean_corrupt_readings_aggressive();
            }
        }
        
        // Intentar reparar timestamps inválidos
        fix_timestamps_in_buffer();
        
        // Ahora limpiar lecturas completamente inválidas
        bool has_invalid_sensor_data = false;
        for (uint16_t i = 0; i < sensor_buffer.count; i++) {
            uint16_t idx = (sensor_buffer.tail + i) % MAX_BUFFER_SIZE;
            stored_reading_t *reading = &sensor_buffer.readings[idx];
            
            bme680_data_t bme_data = {
                .temperature = reading->temperature,
                .humidity = reading->humidity,
                .pressure = reading->pressure,
                .gas_resistance = reading->gas_resistance,
                .air_quality = reading->air_quality
            };
            
            if (!is_sensor_data_valid(&bme_data, reading->rainfall_mm, reading->wind_speed_ms)) {
                ESP_LOGW(TAG, "Eliminando lectura %d con datos de sensores invalidos", idx);
                has_invalid_sensor_data = true;
                
                for (uint16_t j = i + 1; j < sensor_buffer.count; j++) {
                    uint16_t src_idx = (sensor_buffer.tail + j) % MAX_BUFFER_SIZE;
                    uint16_t dst_idx = (sensor_buffer.tail + j - 1) % MAX_BUFFER_SIZE;
                    sensor_buffer.readings[dst_idx] = sensor_buffer.readings[src_idx];
                }
                sensor_buffer.count--;
                i--;
            }
        }
        
        if (has_invalid_sensor_data) {
            ESP_LOGW(TAG, "Limpieza completada. Nuevo count: %d", sensor_buffer.count);
            save_buffer_to_nvs();
        }
    }
    
    buffer_initialized = true;
    ESP_LOGI(TAG, "Buffer listo. Capacidad: %d lecturas", MAX_BUFFER_SIZE);
    
    // Mostrar estado final
    data_buffer_print_status();
}

bool data_buffer_store_reading(bme680_data_t *bme_data, float rainfall_mm, float wind_speed_ms) {
    if (!buffer_initialized) {
        ESP_LOGE(TAG, "Buffer no inicializado");
        return false;
    }
    
    if (!bme_data) {
        ESP_LOGE(TAG, "Datos BME680 nulos");
        return false;
    }
    
    // Validar datos de sensores
    if (!is_sensor_data_valid(bme_data, rainfall_mm, wind_speed_ms)) {
        ESP_LOGW(TAG, "Datos de sensores invalidos, no se almacenan");
        return false;
    }
    
    // Obtener timestamp ACTUAL
    time_t current_time = time_get_current();
    
    // Si el timestamp es muy antiguo, usar tiempo estimado
    if (current_time < 1577836800) {
        uint64_t uptime_sec = esp_timer_get_time() / 1000000ULL;
        current_time = boot_time_reference + uptime_sec;
        ESP_LOGD(TAG, "Usando tiempo estimado para almacenar: %ld", (long)current_time);
    }
    
    // Verificar si hay que hacer espacio
    if (sensor_buffer.buffer_full) {
        ESP_LOGW(TAG, "Buffer lleno, eliminando lectura mas antigua");
        sensor_buffer.tail = (sensor_buffer.tail + 1) % MAX_BUFFER_SIZE;
        sensor_buffer.count--;
        sensor_buffer.buffer_full = false;
    }
    
    bool time_was_synced = time_is_synced();
    
    // Preparar nueva lectura CON TIMESTAMP
    stored_reading_t new_reading;
    
    // Datos del sensor
    new_reading.temperature = bme_data->temperature;
    new_reading.humidity = bme_data->humidity;
    new_reading.pressure = bme_data->pressure;
    new_reading.gas_resistance = bme_data->gas_resistance;
    new_reading.air_quality = bme_data->air_quality;
    new_reading.rainfall_mm = rainfall_mm;
    new_reading.wind_speed_ms = wind_speed_ms;
    
    // Timestamp y metadata
    new_reading.timestamp = current_time;
    new_reading.time_synced = time_was_synced;
    
    // Guardar en buffer
    uint16_t write_idx = sensor_buffer.head;
    sensor_buffer.readings[write_idx] = new_reading;
    sensor_buffer.head = (sensor_buffer.head + 1) % MAX_BUFFER_SIZE;
    sensor_buffer.count++;
    
    // Actualizar estado de lleno
    sensor_buffer.buffer_full = (sensor_buffer.count >= MAX_BUFFER_SIZE);
    
    // Verificar consistencia
    if (sensor_buffer.count > MAX_BUFFER_SIZE) {
        ESP_LOGE(TAG, "ERROR: count (%d) > MAX_BUFFER_SIZE", sensor_buffer.count);
        sensor_buffer.count = MAX_BUFFER_SIZE;
        sensor_buffer.buffer_full = true;
    }
    
    // Convertir timestamp a string legible
    struct tm *timeinfo = localtime(&current_time);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", timeinfo);
    
    ESP_LOGI(TAG, "Almacenado en idx=%d. Total: %d/%d", 
             write_idx, sensor_buffer.count, MAX_BUFFER_SIZE);
    
    // DEBUG: Mostrar datos almacenados
    ESP_LOGD(TAG, "  Hora: %s (%s)", time_str, time_was_synced ? "NTP" : "estimado");
    ESP_LOGD(TAG, "  Temp: %.2fC, Hum: %.2f%%, Pres: %.2fhPa", 
             new_reading.temperature, new_reading.humidity, new_reading.pressure);
    ESP_LOGD(TAG, "  Lluvia: %.2fmm, Viento: %.2fm/s", 
             new_reading.rainfall_mm, new_reading.wind_speed_ms);
    ESP_LOGD(TAG, "  Timestamp Unix: %ld", (long)new_reading.timestamp);
    
    // Guardar periodicamente en flash (cada 10 lecturas)
    static uint8_t save_counter = 0;
    if (++save_counter >= 10) {
        if (save_buffer_to_nvs()) {
            ESP_LOGD(TAG, "Guardado automatico en NVS completado");
        }
        save_counter = 0;
    }
    
    return true;
}

bool data_buffer_send_stored_readings(void) {
	ESP_LOGI(TAG, "=== DEBUG BUFFER COMPLETO ===");
    
    // Mostrar TODAS las lecturas en el buffer
    for (int i = 0; i < sensor_buffer.count; i++) {
        uint16_t idx = (sensor_buffer.tail + i) % MAX_BUFFER_SIZE;
        stored_reading_t *reading = &sensor_buffer.readings[idx];
        
        struct tm *timeinfo = localtime(&reading->timestamp);
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%H:%M:%S", timeinfo);
        
        ESP_LOGI(TAG, "  [%d] %s - T:%.2f, H:%.2f, P:%.2f, R:%.2f, W:%.2f",
                 i, time_str,
                 reading->temperature,
                 reading->humidity,
                 reading->pressure,
                 reading->rainfall_mm,
                 reading->wind_speed_ms);
    }
    
    if (!buffer_initialized || sensor_buffer.count == 0) {
        ESP_LOGI(TAG, "No hay lecturas almacenadas para enviar");
        return true;
    }
    
    ESP_LOGI(TAG, "INICIANDO ENVIO RAPIDO DE DATOS ALMACENADOS");
    ESP_LOGI(TAG, "   Lecturas pendientes: %" PRIu16 "/%d", sensor_buffer.count, MAX_BUFFER_SIZE);
    
    uint16_t sent = 0;
    uint16_t failed = 0;
    uint16_t skipped = 0;
    
    // CONFIGURACION PARA ENVIO RAPIDO (Modo ENVIO DATOS)
    uint16_t max_per_batch = 5;            // Mas lecturas por lote
    uint32_t delay_between_ms = 1000;      // Solo 1 segundo entre envios
    
    while (sensor_buffer.count > 0 && sent < max_per_batch) {
        // Verificar conexion MQTT ANTES de cada envio
        if (!mqtt_is_connected()) {
            ESP_LOGW(TAG, "MQTT desconectado durante envio rapido");
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            
            if (!mqtt_is_connected()) {
                ESP_LOGW(TAG, "MQTT sigue desconectado, abortando");
                break;
            }
            
            ESP_LOGI(TAG, "MQTT reconectado, continuando envio rapido");
        }
        
        // Obtener lectura
        stored_reading_t *reading = &sensor_buffer.readings[sensor_buffer.tail];
        
        // Validar datos de sensores
        bme680_data_t bme_data = {
            .temperature = reading->temperature,
            .humidity = reading->humidity,
            .pressure = reading->pressure,
            .gas_resistance = reading->gas_resistance,
            .air_quality = reading->air_quality
        };
        
        if (!is_sensor_data_valid(&bme_data, reading->rainfall_mm, reading->wind_speed_ms)) {
            ESP_LOGW(TAG, "Lectura invalida, saltando...");
            sensor_buffer.tail = (sensor_buffer.tail + 1) % MAX_BUFFER_SIZE;
            sensor_buffer.count--;
            skipped++;
            continue;
        }
        
        // Si el timestamp es invalido, asignar uno razonable
        time_t send_timestamp = reading->timestamp;
        
        if (send_timestamp < 1577836800) {  // Anterior a 2020
            time_t current_time = time_get_current();
            if (current_time < 1577836800) {
                current_time = 1700000000;  // Referencia segura
            }
            
            // Estimacion simple para envio rapido
            send_timestamp = current_time - (sensor_buffer.count * 5); // 5s entre lecturas
            
            if (send_timestamp < 1577836800) {
                send_timestamp = 1577836800 + sensor_buffer.tail;
            }
        }
        
        // Crear mensaje JSON optimizado para envio rapido
        char json_message[512];
        uint64_t timestamp_ms = (uint64_t)send_timestamp * 1000ULL;
        
        // JSON compacto (menos campos, sin metadata innecesaria para envio rapido)
        int len = snprintf(json_message, sizeof(json_message),
            "{\"ts\":%" PRIu64 ","
            "\"temperature\":%.2f,"      // temperature
            "\"humidity\":%.2f,"      // humidity
            "\"pressure\":%.2f,"      // pressure
            "\"gas_resistance\":%" PRIu32 ","  // gas_resistance
            "\"air_quality\":%.2f,"     // air_quality
            "\"rainfall_mm\":%.2f,"      // rainfall_mm
            "\"wind_speed_ms\":%.2f,"      // wind_speed_ms
            "\"st\":true,"     // stored
            "\"rm\":\"fast\"," // rapid_mode flag
            "\"idx\":%" PRIu16 ","      // buffer_index
            "\"rem\":%" PRIu16 "}",     // remaining_in_buffer
            timestamp_ms,
            reading->temperature,
            reading->humidity,
            reading->pressure,
            reading->gas_resistance,
            reading->air_quality,
            reading->rainfall_mm,
            reading->wind_speed_ms,
            sensor_buffer.tail,
            sensor_buffer.count - 1);
        
        if (len >= sizeof(json_message)) {
            json_message[sizeof(json_message) - 1] = '\0';
        }
        
        // Envio rapido con menos reintentos
        bool success = false;
        int max_attempts = 2;  // Solo 2 reintentos en modo rapido
        
        for (int attempt = 0; attempt < max_attempts && !success; attempt++) {
            if (attempt > 0) {
                uint32_t backoff_ms = 500 * attempt; // Backoff corto: 500ms, 1000ms
                vTaskDelay(backoff_ms / portTICK_PERIOD_MS);
            }
            
            success = send_mqtt_telemetry_with_timestamp(json_message);
            
            if (success) {
                ESP_LOGI(TAG, "Envio rapido exitoso (intento %d)", attempt + 1);
            }
        }
        
        if (success) {
            sent++;
            
            // Actualizar buffer rapidamente
            sensor_buffer.tail = (sensor_buffer.tail + 1) % MAX_BUFFER_SIZE;
            sensor_buffer.count--;
            sensor_buffer.buffer_full = false;
            
            // Pequena pausa entre envios (1 segundo)
            if (sensor_buffer.count > 0 && sent < max_per_batch) {
                vTaskDelay(delay_between_ms / portTICK_PERIOD_MS);
            }
        } else {
            failed++;
            ESP_LOGE(TAG, "Fallo envio rapido");
            
            // Si falla, esperar un poco mas
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            
            // Si falla 2 veces seguidas, salir
            if (failed >= 2) {
                ESP_LOGE(TAG, "Demasiados fallos, abortando envio rapido");
                break;
            }
        }
    }
    
    // Guardar estado si se envio algo
    if (sent > 0 || skipped > 0) {
        save_buffer_to_nvs();
    }
    
    // Reporte rapido
    ESP_LOGI(TAG, "ENVIO RAPIDO COMPLETADO:");
    ESP_LOGI(TAG, "   Enviadas: %" PRIu16 ", Falladas: %" PRIu16 ", Saltadas: %" PRIu16, 
             sent, failed, skipped);
    ESP_LOGI(TAG, "   Restantes: %" PRIu16 "/%d", sensor_buffer.count, MAX_BUFFER_SIZE);
    
    return (failed == 0);
}

uint16_t data_buffer_get_count(void) {
    return sensor_buffer.count;
}

bool data_buffer_is_full(void) {
    return sensor_buffer.buffer_full;
}

void data_buffer_clear(void) {
    ESP_LOGI(TAG, "Limpiando buffer de datos");
    
    sensor_buffer.head = 0;
    sensor_buffer.tail = 0;
    sensor_buffer.count = 0;
    sensor_buffer.buffer_full = false;
    
    nvs_handle_t handle;
    if (nvs_open("sensor_storage", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void data_buffer_print_status(void) {
    ESP_LOGI(TAG, "=== ESTADO DEL BUFFER ===");
    ESP_LOGI(TAG, "Inicializado: %s", buffer_initialized ? "SI" : "NO");
    ESP_LOGI(TAG, "Capacidad: %d lecturas", MAX_BUFFER_SIZE);
    ESP_LOGI(TAG, "Almacenadas: %d (%.1f%%)", 
             sensor_buffer.count, 
             (sensor_buffer.count * 100.0f) / MAX_BUFFER_SIZE);
    ESP_LOGI(TAG, "Lleno: %s", sensor_buffer.buffer_full ? "SI" : "NO");
    ESP_LOGI(TAG, "Head: %d, Tail: %d", sensor_buffer.head, sensor_buffer.tail);
    
    if (sensor_buffer.count > 0) {
        // Mostrar timestamps de las primeras y ultimas lecturas
        stored_reading_t *first = &sensor_buffer.readings[sensor_buffer.tail];
        uint16_t last_idx = (sensor_buffer.head == 0) ? MAX_BUFFER_SIZE - 1 : sensor_buffer.head - 1;
        stored_reading_t *last = &sensor_buffer.readings[last_idx];
        
        struct tm *first_time = localtime(&first->timestamp);
        struct tm *last_time = localtime(&last->timestamp);
        char first_str[32], last_str[32];
        strftime(first_str, sizeof(first_str), "%H:%M:%S %d/%m", first_time);
        strftime(last_str, sizeof(last_str), "%H:%M:%S %d/%m", last_time);
        
        ESP_LOGI(TAG, "Rango temporal:");
        ESP_LOGI(TAG, "  Primera: %s (%s)", first_str, first->time_synced ? "NTP" : "estimado");
        ESP_LOGI(TAG, "  Ultima:  %s (%s)", last_str, last->time_synced ? "NTP" : "estimado");
    }
}
/**
 * @brief Repara o elimina lecturas corruptas del buffer
 * 
 * Identifica y elimina lecturas que tienen valores inválidos como:
 * - Temperatura = 0.0 (o muy cercana a 0)
 * - Presión = 0.0
 * - Todos los valores en 0
 * - Valores fuera de rango físico
 * 
 * @return Número de lecturas reparadas/eliminadas
 */
int data_buffer_repair_corrupt_entries(void) {
    if (!buffer_initialized || sensor_buffer.count == 0) {
        ESP_LOGI(TAG, "Buffer vacío o no inicializado, nada que reparar");
        return 0;
    }
    
    ESP_LOGI(TAG, "=== REPARACIÓN DE BUFFER CORRUPTO ===");
    ESP_LOGI(TAG, "Analizando %d lecturas...", sensor_buffer.count);
    
    int repaired_count = 0;
    int deleted_count = 0;
    int scanned_count = 0;
    
    // Analizar todas las lecturas
    for (int i = 0; i < sensor_buffer.count; i++) {
        uint16_t idx = (sensor_buffer.tail + i) % MAX_BUFFER_SIZE;
        stored_reading_t *reading = &sensor_buffer.readings[idx];
        
        scanned_count++;
        bool is_corrupt = false;
        char reason[64] = {0};
        
        // ========== DETECCIÓN DE CORRUPCIÓN ==========
        
        // 1. Detectar TODOS los valores en 0 o muy cercanos a 0
        if (fabs(reading->temperature) < 0.1f && 
            fabs(reading->humidity) < 0.1f && 
            fabs(reading->pressure) < 0.1f &&
            fabs(reading->rainfall_mm) < 0.1f &&
            fabs(reading->wind_speed_ms) < 0.1f) {
            is_corrupt = true;
            snprintf(reason, sizeof(reason), "Todos los valores en 0");
        }
        // 2. Temperatura fuera de rango físico (-50°C a +100°C)
        else if (reading->temperature < -50.0f || reading->temperature > 100.0f) {
            is_corrupt = true;
            snprintf(reason, sizeof(reason), "Temp fuera rango: %.2fC", reading->temperature);
        }
        // 3. Temperatura = 0 exacto (imposible en condiciones normales)
        else if (fabs(reading->temperature) < 0.01f) {
            is_corrupt = true;
            snprintf(reason, sizeof(reason), "Temp=0.00C exacto");
        }
        // 4. Presión = 0 (imposible)
        else if (fabs(reading->pressure) < 0.01f) {
            is_corrupt = true;
            snprintf(reason, sizeof(reason), "Presion=0.00hPa");
        }
        // 5. Humedad fuera de rango (0-100%)
        else if (reading->humidity < 0.0f || reading->humidity > 100.0f) {
            is_corrupt = true;
            snprintf(reason, sizeof(reason), "Hum fuera rango: %.2f%%", reading->humidity);
        }
        // 6. Timestamp muy antiguo (< 2020) o futuro (> +24 horas)
        else if (!time_is_timestamp_valid(reading->timestamp)) {
            is_corrupt = true;
            struct tm *timeinfo = localtime(&reading->timestamp);
            char time_str[32];
            strftime(time_str, sizeof(time_str), "%H:%M:%S %d/%m/%Y", timeinfo);
            snprintf(reason, sizeof(reason), "Timestamp invalido: %s", time_str);
        }
        
        // ========== ACCIÓN CORRECTIVA ==========
        
        if (is_corrupt) {
            ESP_LOGW(TAG, "  [%d] CORRUPTO: %s", idx, reason);
            ESP_LOGW(TAG, "    Valores: T=%.2f, H=%.2f, P=%.2f, R=%.2f, W=%.2f",
                     reading->temperature, reading->humidity, reading->pressure,
                     reading->rainfall_mm, reading->wind_speed_ms);
            
            // Intentar REPARAR si es posible (solo algunos campos malos)
            bool can_repair = false;
            
            // Caso 1: Solo timestamp malo pero datos OK
            if (!time_is_timestamp_valid(reading->timestamp) &&
                reading->temperature > -50.0f && reading->temperature < 100.0f &&
                reading->humidity >= 0.0f && reading->humidity <= 100.0f &&
                reading->pressure >= 300.0f && reading->pressure <= 1100.0f) {
                
                // Reparar timestamp estimado
                time_t current_time = time_get_validated_current();
                time_t estimated_time = current_time - ((sensor_buffer.count - i) * 10); // 10s entre lecturas
                
                if (estimated_time < 1577836800) {
                    estimated_time = 1577836800 + i; // Mínimo 2020
                }
                
                reading->timestamp = estimated_time;
                reading->time_synced = false;
                
                struct tm *new_timeinfo = localtime(&estimated_time);
                char new_time_str[32];
                strftime(new_time_str, sizeof(new_time_str), "%H:%M:%S", new_timeinfo);
                
                ESP_LOGI(TAG, "    ✅ REPARADO: timestamp %ld -> %ld (%s)", 
                         (long)reading->timestamp, (long)estimated_time, new_time_str);
                
                repaired_count++;
                can_repair = true;
            }
            
            // Si no se puede reparar, ELIMINAR
            if (!can_repair) {
                // Desplazar todas las lecturas posteriores
                for (int j = i + 1; j < sensor_buffer.count; j++) {
                    uint16_t src_idx = (sensor_buffer.tail + j) % MAX_BUFFER_SIZE;
                    uint16_t dst_idx = (sensor_buffer.tail + j - 1) % MAX_BUFFER_SIZE;
                    sensor_buffer.readings[dst_idx] = sensor_buffer.readings[src_idx];
                }
                
                deleted_count++;
                sensor_buffer.count--;
                i--; // Revisar la misma posición de nuevo (ahora tiene la siguiente lectura)
                
                // Ajustar índices si se eliminó del final
                if (sensor_buffer.head > 0) {
                    sensor_buffer.head--;
                } else {
                    sensor_buffer.head = MAX_BUFFER_SIZE - 1;
                }
                
                ESP_LOGI(TAG, "    ❌ ELIMINADO: lectura %d eliminada", idx);
            }
        } else {
            // Lectura válida - mostrar para diagnóstico
            if (i < 5) { // Mostrar solo primeras 5 para no saturar log
                struct tm *timeinfo = localtime(&reading->timestamp);
                char time_str[32];
                strftime(time_str, sizeof(time_str), "%H:%M:%S", timeinfo);
                
                ESP_LOGI(TAG, "  [%d] VÁLIDO: %s - T=%.2fC, H=%.2f%%, P=%.2fhPa",
                         idx, time_str, 
                         reading->temperature, reading->humidity, reading->pressure);
            }
        }
    }
    
    // Actualizar estado del buffer
    sensor_buffer.buffer_full = (sensor_buffer.count >= MAX_BUFFER_SIZE);
    
    // Guardar cambios en NVS
    if (repaired_count > 0 || deleted_count > 0) {
        if (save_buffer_to_nvs()) {
            ESP_LOGI(TAG, "Cambios guardados en NVS");
        }
    }
    
    // Reporte final
    ESP_LOGI(TAG, "=== RESUMEN REPARACIÓN ===");
    ESP_LOGI(TAG, "Lecturas analizadas: %d", scanned_count);
    ESP_LOGI(TAG, "Lecturas reparadas:  %d", repaired_count);
    ESP_LOGI(TAG, "Lecturas eliminadas: %d", deleted_count);
    ESP_LOGI(TAG, "Lecturas restantes:  %d", sensor_buffer.count);
    ESP_LOGI(TAG, "Estado buffer: %s", 
             sensor_buffer.buffer_full ? "LLENO" : 
             sensor_buffer.count == 0 ? "VACÍO" : "PARCIAL");
    ESP_LOGI(TAG, "==========================");
    
    return repaired_count + deleted_count;
}

