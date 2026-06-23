#include "system_config.h"

static const char *TAG = "MAIN_SYSTEM";

// =============================================================================
// ESTRUCTURAS DE ESTADO SIMPLIFICADAS
// =============================================================================

typedef struct {
    uint32_t total_readings;
    uint32_t stored_readings;
    uint32_t sent_readings;
    uint32_t failed_readings;
    uint32_t last_wifi_check;
    uint32_t last_diagnostic;
    int cycle_count;
    bool was_connected;
    bool system_initialized;
    bool sending_buffered_data;  // Nueva bandera para controlar envío de buffer
    uint8_t buffer_send_cycles;  // Contador de ciclos dedicados a vaciar buffer
} system_state_t;

static system_state_t sys_state = {0};

typedef struct {
    bme680_data_t bme_data;
    float rainfall_mm;
    float wind_speed_ms;
    bool bme_valid;
    bool weather_valid;
    bool mqtt_connected;
    bool wifi_connected;
} sensor_readings_t;

// =============================================================================
// FUNCIONES AUXILIARES
// =============================================================================

static void system_initialize(void) {
    ESP_LOGI(TAG, "🚀 Iniciando Estación Meteorológica");

    // 0. Gestión de energía: permite que la CPU baje de frecuencia y entre en
    // light-sleep automáticamente en los huecos de inactividad (tickless idle),
    // sin afectar a WiFi/MQTT/portal, que siguen funcionando con normalidad.
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = true,
    };
    esp_err_t pm_ret = esp_pm_configure(&pm_config);
    if (pm_ret != ESP_OK) {
        ESP_LOGW(TAG, "⚠️  No se pudo configurar power management: %s", esp_err_to_name(pm_ret));
    }

    // 1. Inicializar NVS
    ESP_LOGI(TAG, "📁 Inicializando NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    
	// 5. Inicializar WiFi
    ESP_LOGI(TAG, "📡 Inicializando WiFi...");
    wifi_init_sta();
    
    if (wifi_is_connected()) {
        ESP_LOGI(TAG, "📤 Inicializando MQTT...");
        mqtt_init();
        ESP_LOGI(TAG, "🕐 Inicializando sistema de tiempo...");
    	time_init();
    }
    
    // 3. Inicializar buffer de datos (AHORA con sistema de tiempo listo)
    ESP_LOGI(TAG, "💾 Inicializando buffer de datos...");
    data_buffer_init();
    
    // 4. Inicializar BME680
    ESP_LOGI(TAG, "🌡️  Inicializando sensor BME680...");
    bme680_init();
    if (bme680_configure_sensor() != ESP_OK) {
        ESP_LOGW(TAG, "⚠️  Problema con BME680, continuando...");
    }
    
    // 6. Inicializar sensores meteorológicos
    ESP_LOGI(TAG, "🌧️  Inicializando sensores meteorológicos...");
    init_sensors();
    
    // 7. IMPORTANTE: NO almacenar lecturas hasta que el tiempo esté sincronizado
    //    Para eso necesitamos una bandera global
    
    // 8. Inicializar MQTT si hay WiFi
    if (wifi_is_connected()) {
        ESP_LOGI(TAG, "📤 Inicializando MQTT...");
        mqtt_init();
    }
    
    sys_state.system_initialized = true;
    ESP_LOGI(TAG, "✅ Sistema inicializado completamente");
}

static void read_all_sensors(sensor_readings_t *readings) {
    if (!readings) return;
    
    // 1. Leer BME680
    esp_err_t bme_result = bme680_read_all_data(&readings->bme_data);
    
    if (bme_result != ESP_OK) {
        ESP_LOGW(TAG, "⚠️  Error leyendo BME680, usando valores por defecto");
        readings->bme_data.temperature = 25.0;
        readings->bme_data.humidity = 50.0;
        readings->bme_data.pressure = 1013.0;
        readings->bme_valid = false;
    } else {
        readings->bme_valid = true;
    }
    
    // 2. Leer sensores meteorológicos
    readings->rainfall_mm = read_pluviometro_value();
    readings->wind_speed_ms = read_anemometro_value();
    readings->weather_valid = (readings->rainfall_mm >= 0 && readings->wind_speed_ms >= 0);
    
    // 3. Verificar conexiones
    readings->wifi_connected = wifi_is_connected();
    readings->mqtt_connected = mqtt_is_connected();
    
    // ⭐⭐ NUEVO: Verificar periódicamente MQTT ⭐⭐
    static uint8_t check_counter = 0;
    if (++check_counter >= 10) {  // Cada 10 ciclos (~100 segundos)
        mqtt_check_and_reconnect();
        check_counter = 0;
    }
    
    sys_state.total_readings++;
}

static int calculate_wait_time(bool is_connected, bool sending_buffer) {
    if (sending_buffer) {
        // Cuando estamos vaciando buffer, esperar menos tiempo
        return 3;  // 3 segundos entre envíos de buffer
    } else if (is_connected) {
        // ThingsBoard aguanta ~10 mensajes/minuto como máximo
        // Enviamos cada 10 segundos = 6/minuto (seguro)
        return 10;
    } else {
        return 5;
    }
}

static void handle_buffer_empty_mode(sensor_readings_t *readings) {
    ESP_LOGI(TAG, "=== CICLO %d (CONECTADO - BUFFER VACÍO) ===", sys_state.cycle_count);
    
    // 1. Leer y enviar datos actuales si son válidos
    if (readings->bme_valid && readings->weather_valid) {
        ESP_LOGI(TAG, "📤 Enviando datos actuales a la nube...");
        send_mqtt_telemetry(&readings->bme_data, readings->rainfall_mm, readings->wind_speed_ms);
        sys_state.sent_readings++;
        
        ESP_LOGI(TAG, "✅ Datos actuales enviados. Esperando %d segundos...", 
                calculate_wait_time(true, false));
    } else {
        ESP_LOGW(TAG, "⚠️  Datos de sensores no válidos, omitiendo envío");
    }
    
    // 2. Checkeo de buffer (solo cada 3 ciclos para no saturar)
    if (sys_state.cycle_count % 3 == 0) {
        uint16_t buffer_count = data_buffer_get_count();
        if (buffer_count > 0) {
            ESP_LOGI(TAG, "📦 Se detectaron %d lecturas en buffer, cambiando a modo vaciado", 
                    buffer_count);
            sys_state.sending_buffered_data = true;
            sys_state.buffer_send_cycles = 0;
        }
    }
}

static void handle_buffer_send_mode(sensor_readings_t *readings) {
    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
    ESP_LOGI(TAG, "📦 MODO VACIADO DE BUFFER - CICLO %d", sys_state.cycle_count);
    ESP_LOGI(TAG, "   Buffer pendiente: %d/%d lecturas", 
            data_buffer_get_count(), MAX_BUFFER_SIZE);
    ESP_LOGI(TAG, "   Ciclos dedicados a vaciado: %d", sys_state.buffer_send_cycles);
    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
    
    // 1. Primero enviar datos del buffer
    bool buffer_sent = data_buffer_send_stored_readings();
    
    if (!buffer_sent) {
        // Si falla, intentar limpiar datos corruptos
        ESP_LOGW(TAG, "⚠️ Fallo envío, verificando datos corruptos...");
        int corrupt_count = data_buffer_repair_corrupt_entries();
        
        if (corrupt_count > 0) {
            ESP_LOGI(TAG, "Reparadas %d lecturas, reintentando envío...", corrupt_count);
            buffer_sent = data_buffer_send_stored_readings();
        }
    }
    
    // 2. Verificar si todavía hay datos en el buffer
    uint16_t remaining = data_buffer_get_count();
    
    if (remaining == 0) {
        // Buffer vacío, volver a modo normal
        ESP_LOGI(TAG, "🎉 ¡BUFFER VACÍO COMPLETAMENTE!");
        ESP_LOGI(TAG, "   Total ciclos dedicados: %d", sys_state.buffer_send_cycles);
        ESP_LOGI(TAG, "   Volviendo a modo normal de lectura...");
        sys_state.sending_buffered_data = false;
        sys_state.buffer_send_cycles = 0;
    } else {
        // Todavía hay datos, seguir en modo vaciado
        sys_state.buffer_send_cycles++;
        
        // Prevenir bloqueo infinito (máximo 20 ciclos seguidos)
        if (sys_state.buffer_send_cycles > 20) {
            ESP_LOGW(TAG, "⚠️  Límite de ciclos de vaciado alcanzado (20)");
            ESP_LOGW(TAG, "   Buffer restante: %d lecturas", remaining);
            ESP_LOGW(TAG, "   Volviendo a modo normal temporalmente...");
            sys_state.sending_buffered_data = false;
            sys_state.buffer_send_cycles = 0;
        }
    }
    
    // 3. Aún en modo vaciado, también leer sensores actuales PERO NO ALMACENAR
    // (solo para monitoreo, no se envían para no saturar)
    if (readings->bme_valid && readings->weather_valid) {
        ESP_LOGI(TAG, "📊 Monitoreo sensores (no se envían):");
        ESP_LOGI(TAG, "   Temp: %.2f°C, Hum: %.2f%%, Pres: %.2fhPa", 
                readings->bme_data.temperature, 
                readings->bme_data.humidity, 
                readings->bme_data.pressure);
        ESP_LOGI(TAG, "   Lluvia: %.2fmm, Viento: %.2fm/s", 
                readings->rainfall_mm, readings->wind_speed_ms);
    }
}

static void handle_connected_mode(sensor_readings_t *readings) {
    // Verificar si estamos en modo vaciado de buffer
    if (sys_state.sending_buffered_data) {
        handle_buffer_send_mode(readings);
    } else {
        // Verificar si hay datos en el buffer
        uint16_t buffer_count = data_buffer_get_count();
        
        if (buffer_count > 0) {
            // Hay datos en buffer, activar modo vaciado
            ESP_LOGI(TAG, "📦 Buffer con %d lecturas pendientes", buffer_count);
            ESP_LOGI(TAG, "🚀 Activando modo vaciado de buffer...");
            sys_state.sending_buffered_data = true;
            sys_state.buffer_send_cycles = 0;
            handle_buffer_send_mode(readings);
        } else {
            // Buffer vacío, modo normal
            handle_buffer_empty_mode(readings);
        }
    }
    
    // ⭐⭐ NUEVO: Detectar reconexión WiFi y forzar reconexión MQTT ⭐⭐
    if (!sys_state.was_connected) {
        ESP_LOGI(TAG, "🔄 ¡RECONEXIÓN DETECTADA!");
        
        // Esperar un momento para que la conexión WiFi sea estable
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        
        // Forzar reconexión MQTT si no está conectado
        if (!readings->mqtt_connected) {
            ESP_LOGI(TAG, "📡 WiFi reconectado pero MQTT no, forzando reconexión...");
            mqtt_force_reconnect();
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
    }
}

static void handle_disconnected_mode(sensor_readings_t *readings) {
    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
    ESP_LOGI(TAG, "🔌 MODO DESCONECTADO - CICLO %d", sys_state.cycle_count);
    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
    
    // Mostrar hora actual
    char time_str[32];
    time_t now = time_get_current();
    struct tm *timeinfo = localtime(&now);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", timeinfo);
    ESP_LOGI(TAG, "🕐 Hora actual: %s (%s)", 
             time_str, time_is_synced() ? "NTP" : "estimada");
    
    // Mostrar estado de conexión
    ESP_LOGI(TAG, "📡 WiFi: %s | MQTT: %s",
             readings->wifi_connected ? "✅" : "❌",
             readings->mqtt_connected ? "✅" : "❌");
    
    // Mostrar TODOS los datos en el MISMO formato
    if (readings->bme_valid && readings->weather_valid) {
        ESP_LOGI(TAG, "═══════════════════════════════════════════════");
        ESP_LOGI(TAG, "📊 LECTURA DE SENSORES LOCAL");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════");
        
        ESP_LOGI(TAG, "🌡️  TEMPERATURA: %.2f °C", readings->bme_data.temperature);
        ESP_LOGI(TAG, "💧 HUMEDAD: %.2f %%", readings->bme_data.humidity);
        ESP_LOGI(TAG, "📊 PRESIÓN: %.2f hPa", readings->bme_data.pressure);
        ESP_LOGI(TAG, "🌀 RESISTENCIA GAS: %lu Ω", 
                 (unsigned long)readings->bme_data.gas_resistance);
        ESP_LOGI(TAG, "🌬️  CALIDAD AIRE: %.2f /100", readings->bme_data.air_quality);
        ESP_LOGI(TAG, "🌧️  LLUVIA ACUMULADA: %.2f mm", readings->rainfall_mm);
        ESP_LOGI(TAG, "💨 VELOCIDAD VIENTO: %.2f m/s (%.2f km/h)",
                 readings->wind_speed_ms, readings->wind_speed_ms * 3.6);
        
        ESP_LOGI(TAG, "═══════════════════════════════════════════════");
        
        // Intentar almacenar en buffer
        bool stored = data_buffer_store_reading(&readings->bme_data, 
                                               readings->rainfall_mm, 
                                               readings->wind_speed_ms);
        
        if (stored) {
            sys_state.stored_readings++;
            uint16_t buffer_count = data_buffer_get_count();
            
            ESP_LOGI(TAG, "✅ LECTURA ALMACENADA EN BUFFER LOCAL");
            ESP_LOGI(TAG, "   Total en buffer: %d/%d", buffer_count, MAX_BUFFER_SIZE);
            ESP_LOGI(TAG, "   Porcentaje: %.1f%%", 
                     (buffer_count * 100.0f) / MAX_BUFFER_SIZE);
            
            if (buffer_count > MAX_BUFFER_SIZE * 0.8) {
                ESP_LOGW(TAG, "⚠️  Buffer casi lleno (>80%%)");
            }
        } else {
            ESP_LOGW(TAG, "⚠️  No se pudo almacenar (datos duplicados o inválidos)");
        }
    } else {
        ESP_LOGW(TAG, "❌ Datos de sensores no válidos:");
        ESP_LOGW(TAG, "   BME680 válido: %s", readings->bme_valid ? "SÍ" : "NO");
        ESP_LOGW(TAG, "   Sensores meteo válidos: %s", readings->weather_valid ? "SÍ" : "NO");
    }
    
    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
    ESP_LOGI(TAG, "⏳ Esperando %d segundos para siguiente lectura...",
             calculate_wait_time(false, false));
    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
}

static void show_system_report(void) {
    if (sys_state.cycle_count % 10 == 0) {
        ESP_LOGI(TAG, "📊 INFORME [Ciclo %d]", sys_state.cycle_count);
        ESP_LOGI(TAG, "   WiFi: %s, MQTT: %s, Modo: %s",
                 wifi_is_connected() ? "✅" : "❌",
                 mqtt_is_connected() ? "✅" : "❌",
                 sys_state.sending_buffered_data ? "VACIADO BUFFER" : "NORMAL");
        ESP_LOGI(TAG, "   Buffer: %d/%d lecturas (%.1f%%)",
                 data_buffer_get_count(), MAX_BUFFER_SIZE,
                 (data_buffer_get_count() * 100.0f) / MAX_BUFFER_SIZE);
        ESP_LOGI(TAG, "   Lecturas: Total=%"PRIu32" Env=%"PRIu32", Alm=%"PRIu32"",
                 sys_state.total_readings,
                 sys_state.sent_readings,
                 sys_state.stored_readings);
        
        if (sys_state.sending_buffered_data) {
            ESP_LOGI(TAG, "   Ciclos vaciado: %d/20", sys_state.buffer_send_cycles);
        }
    }
}


// =============================================================================
// FUNCIÓN PRINCIPAL
// =============================================================================

void app_main(void) {
    // 1. Inicializar sistema
    system_initialize();
    
    // 2. Loop principal
    ESP_LOGI(TAG, "🔄 Iniciando loop principal...");
    
    while (1) {
        sys_state.cycle_count++;
        
        // A. Leer sensores (siempre leer para monitoreo)
        sensor_readings_t readings;
        read_all_sensors(&readings);
        
        // B. Determinar modo (conectado/desconectado)
        bool is_connected = readings.wifi_connected && readings.mqtt_connected;
        
        // C. Manejar según modo
        if (is_connected) {
            handle_connected_mode(&readings);
        } else {
            handle_disconnected_mode(&readings);
        }
        
        // D. Tareas de mantenimiento
        show_system_report();

        // D.1 Comprobar actualizaciones OTA periódicamente (solo si hay WiFi)
        if (is_connected && sys_state.cycle_count % OTA_CHECK_INTERVAL_CYCLES == 0) {
            check_ota_updates();
        }
        
        // E. Actualizar estado anterior
        sys_state.was_connected = is_connected;
        
        // F. Espera
        int wait_time = calculate_wait_time(is_connected, sys_state.sending_buffered_data);
        ESP_LOGI(TAG, "⏱️  Esperando %d segundos...", wait_time);
        vTaskDelay(wait_time * 1000 / portTICK_PERIOD_MS);
    }
}