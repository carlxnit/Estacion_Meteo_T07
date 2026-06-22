#include "esp_sntp.h"
#include "system_config.h"
#include <time.h>


static const char *TAG = "TIME_MANAGER";

// =============================================================================
// VARIABLES PRIVADAS
// =============================================================================
static bool time_synced = false;                 // ¿Hora sincronizada con NTP?
static time_t boot_time_reference = 0;           // Tiempo Unix al momento del boot
static uint32_t last_sync_attempt = 0;           // Último intento de sincronización

// =============================================================================
// FUNCIONES PRIVADAS
// =============================================================================

/**
 * @brief Callback cuando NTP sincroniza exitosamente
 */
static void time_sync_notification_cb(struct timeval *tv) {
    time_synced = true;
    boot_time_reference = tv->tv_sec - (esp_timer_get_time() / 1000000ULL);
    
    // Convertir a string legible
    struct tm *timeinfo = localtime(&tv->tv_sec);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S %d/%m/%Y", timeinfo);
    
    ESP_LOGI(TAG, "✅ Hora sincronizada con NTP: %s", time_str);
}

/**
 * @brief Verifica si ya pasó suficiente tiempo desde el último intento de sync
 */
static bool time_should_sync_again(void) {
    if (last_sync_attempt == 0) {
        return true;  // Nunca se intentó
    }
    
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t elapsed = now - last_sync_attempt;
    
    // Esperar al menos 5 minutos entre intentos
    return (elapsed > 300);
}

// =============================================================================
// FUNCIONES PÚBLICAS
// =============================================================================

/**
 * @brief Inicializa el sistema de tiempo
 * 
 * Configura zona horaria, servidores NTP y prepara el sistema.
 * Si hay WiFi disponible, intenta sincronizar automáticamente.
 */
void time_init(void) {
    ESP_LOGI(TAG, "🕐 Inicializando sistema de tiempo...");
    
    // 1. Configurar zona horaria
    ESP_LOGI(TAG, "Zona horaria: %s", TIME_ZONE);
    setenv("TZ", TIME_ZONE, 1);
    tzset();
    
    // 2. Configurar servidores NTP
    ESP_LOGI(TAG, "Configurando servidores NTP:");
    ESP_LOGI(TAG, "  • %s", NTP_SERVER1);
    ESP_LOGI(TAG, "  • %s", NTP_SERVER2);
    ESP_LOGI(TAG, "  • %s", NTP_SERVER3);
    
    // 3. Inicializar SNTP en modo polling
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    // 4. Establecer servidores NTP
    esp_sntp_setservername(0, NTP_SERVER1);
    esp_sntp_setservername(1, NTP_SERVER2);
    esp_sntp_setservername(2, NTP_SERVER3);
    
    // 5. Configurar callback de sincronización
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    
    // 6. Iniciar servicio SNTP
    esp_sntp_init();
    
    // 7. Establecer tiempo de referencia inicial (1 Enero 2025)
    boot_time_reference = 1735689600;  // 2025-01-01 00:00:00 UTC
    
    ESP_LOGI(TAG, "✅ Sistema de tiempo inicializado");
    ESP_LOGI(TAG, "Tiempo inicial estimado: %s", time_get_current_str());
}

/**
 * @brief Sincroniza la hora con servidores NTP
 * 
 * @return true si la sincronización fue exitosa, false en caso contrario
 * 
 * Nota: Esta función es bloqueante y puede tardar hasta NTP_SYNC_TIMEOUT_MS
 */
bool time_sync_with_ntp(void) {
    // Verificar si hay conexión WiFi
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "❌ No hay conexión WiFi para sincronizar NTP");
        return false;
    }
    
    // Verificar si ya se intentó hace poco
    if (!time_should_sync_again()) {
        ESP_LOGD(TAG, "Sincronización NTP reciente, omitiendo...");
        return time_synced;
    }
    
    ESP_LOGI(TAG, "⏳ Sincronizando con servidores NTP...");
    last_sync_attempt = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    
    // Esperar sincronización con timeout
    int retry = 0;
    bool sync_completed = false;
    
    while (retry < (NTP_SYNC_TIMEOUT_MS / 1000) && !sync_completed) {
        // Verificar estado de sincronización
        switch (sntp_get_sync_status()) {
            case SNTP_SYNC_STATUS_COMPLETED:
                sync_completed = true;
                time_synced = true;
                break;
                
            case SNTP_SYNC_STATUS_IN_PROGRESS:
                ESP_LOGD(TAG, "Sincronización NTP en progreso... (%d/%d)", 
                        retry + 1, NTP_SYNC_TIMEOUT_MS / 1000);
                break;
                
            case SNTP_SYNC_STATUS_RESET:
                ESP_LOGD(TAG, "Sincronización NTP reiniciada");
                break;
        }
        
        if (!sync_completed) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            retry++;
        }
    }
    
    // Resultado
    if (sync_completed) {
        ESP_LOGI(TAG, "✅ Sincronización NTP exitosa en %d segundos", retry);
        ESP_LOGI(TAG, "Hora actual: %s", time_get_current_str());
        return true;
    } else {
        ESP_LOGW(TAG, "❌ Timeout de sincronización NTP (%d segundos)", retry);
        return false;
    }
}

/**
 * @brief Verifica si la hora está sincronizada con NTP
 * 
 * @return true si la hora está sincronizada, false en caso contrario
 */
bool time_is_synced(void) {
    return time_synced;
}

/**
 * @brief Obtiene el tiempo actual en segundos Unix
 * 
 * @return Segundos desde epoch Unix (1 Enero 1970 00:00:00 UTC)
 * 
 * Si la hora está sincronizada con NTP, devuelve el tiempo real.
 * Si no está sincronizada, estima el tiempo basado en el tiempo de boot.
 */
time_t time_get_current(void) {
    if (time_synced) {
        // Hora sincronizada con NTP - usar time() del sistema
        return time(NULL);
    } else {
        // Estimar tiempo basado en referencia de boot + uptime
        uint64_t uptime_sec = esp_timer_get_time() / 1000000ULL;
        return boot_time_reference + uptime_sec;
    }
}

/**
 * @brief Obtiene el tiempo actual en milisegundos Unix
 * 
 * @return Milisegundos desde epoch Unix (1 Enero 1970 00:00:00 UTC)
 */
uint64_t time_get_current_ms(void) {
    if (time_synced) {
        // Hora sincronizada - usar gettimeofday para mayor precisión
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
    } else {
        // Estimación con microsegundos de precisión
        uint64_t uptime_us = esp_timer_get_time();  // Microsegundos desde boot
        uint64_t uptime_ms = uptime_us / 1000ULL;
        return (uint64_t)boot_time_reference * 1000ULL + uptime_ms;
    }
}

/**
 * @brief Obtiene el tiempo actual como string legible
 * 
 * @return String con formato "Wed Jan  1 12:00:00 2025"
 * 
 * El string es estático, no es thread-safe para múltiples llamadas simultáneas.
 */
const char* time_get_current_str(void) {
    static char time_str[64];
    time_t now = time_get_current();
    
    if (now < 1600000000) {  // Si el tiempo es anterior a 2020
        snprintf(time_str, sizeof(time_str), "NO_SINCRONIZADO");
        return time_str;
    }
    
    struct tm *timeinfo = localtime(&now);
    
    if (timeinfo == NULL) {
        snprintf(time_str, sizeof(time_str), "ERROR_TIEMPO");
        return time_str;
    }
    
    strftime(time_str, sizeof(time_str), "%a %b %d %H:%M:%S %Y", timeinfo);
    return time_str;
}

/**
 * @brief Obtiene el tiempo actual como string corto (solo hora)
 * 
 * @return String con formato "12:00:00"
 */
const char* time_get_current_time_str(void) {
    static char time_str[16];
    time_t now = time_get_current();
    struct tm *timeinfo = localtime(&now);
    
    if (timeinfo == NULL) {
        snprintf(time_str, sizeof(time_str), "--:--:--");
        return time_str;
    }
    
    strftime(time_str, sizeof(time_str), "%H:%M:%S", timeinfo);
    return time_str;
}

/**
 * @brief Obtiene el tiempo actual como string de fecha
 * 
 * @return String con formato "2025-01-01"
 */
const char* time_get_current_date_str(void) {
    static char date_str[16];
    time_t now = time_get_current();
    struct tm *timeinfo = localtime(&now);
    
    if (timeinfo == NULL) {
        snprintf(date_str, sizeof(date_str), "0000-00-00");
        return date_str;
    }
    
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", timeinfo);
    return date_str;
}

/**
 * @brief Establece la hora manualmente (sin NTP)
 * 
 * @param manual_time Tiempo Unix a establecer (segundos desde epoch)
 * 
 * Útil para configurar la hora cuando no hay conexión WiFi disponible.
 */
void time_set_manual(time_t manual_time) {
    // Validar que el tiempo sea razonable (posterior a 2020)
    if (manual_time < 1600000000) {
        ESP_LOGW(TAG, "⚠️  Tiempo manual muy antiguo: %lld", manual_time);
        return;
    }
    
    ESP_LOGI(TAG, "🕐 Estableciendo hora manual: %lld", manual_time);
    
    // Establecer tiempo del sistema
    struct timeval tv = {
        .tv_sec = manual_time,
        .tv_usec = 0
    };
    
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE(TAG, "❌ Error estableciendo hora manual");
        return;
    }
    
    // Actualizar referencia de boot
    boot_time_reference = manual_time - (esp_timer_get_time() / 1000000ULL);
    time_synced = false;  // Marcar como no sincronizado con NTP
    
    ESP_LOGI(TAG, "✅ Hora manual establecida: %s", time_get_current_str());
}

/**
 * @brief Obtiene información del estado del sistema de tiempo
 * 
 * Imprime en logs el estado actual: sincronización, hora actual, etc.
 */
void time_print_status(void) {
    ESP_LOGI(TAG, "=== ESTADO SISTEMA DE TIEMPO ===");
    ESP_LOGI(TAG, "Sincronizado con NTP: %s", time_synced ? "✅ SÍ" : "❌ NO");
    ESP_LOGI(TAG, "Hora actual: %s", time_get_current_str());
    ESP_LOGI(TAG, "Timestamp Unix: %lld segundos", time_get_current());
    ESP_LOGI(TAG, "Timestamp ms: %llu milisegundos", time_get_current_ms());
    
    if (!time_synced) {
        ESP_LOGW(TAG, "⚠️  Hora estimada - puede tener desviación");
        ESP_LOGW(TAG, "   Último sync intentado hace: %"PRIu32" segundos", 
                last_sync_attempt > 0 ? 
                (uint32_t)(esp_timer_get_time() / 1000000ULL) - last_sync_attempt : 0);
    }
    
    ESP_LOGI(TAG, "=================================");
}

/**
 * @brief Intenta sincronizar si hay WiFi y hace tiempo que no se sincroniza
 * 
 * Función no bloqueante para llamar periódicamente desde el loop principal.
 */
void time_try_sync_periodic(void) {
    // Solo intentar si hay WiFi y hace más de 5 minutos
    if (wifi_is_connected() && time_should_sync_again()) {
        ESP_LOGI(TAG, "🔄 Intentando sincronización periódica NTP...");
        
        // Iniciar sync en segundo plano (no bloqueante)
        if (time_sync_with_ntp()) {
            ESP_LOGI(TAG, "✅ Sincronización periódica exitosa");
        } else {
            ESP_LOGW(TAG, "⚠️  Sincronización periódica falló");
        }
    }
}

/**
 * @brief Valida que un timestamp sea razonable (no futuro)
 * 
 * @param timestamp Tiempo a validar en segundos Unix
 * @return true si el timestamp es razonable, false si es futuro o muy antiguo
 */
bool time_is_timestamp_valid(time_t timestamp) {
    // Obtener tiempo actual REAL del sistema
    time_t current_system_time = time(NULL);
    
    // Si el timestamp es 0, es inválido
    if (timestamp == 0) {
        return false;
    }
    
    // Si el timestamp es anterior a 2020, es muy antiguo
    if (timestamp < 1577836800) {  // 2020-01-01
        return false;
    }
    
    // Si el timestamp es más de 24 horas en el futuro, es inválido
    if (timestamp > (current_system_time + 86400)) {  // +24 horas
        return false;
    }
    
    return true;
}

/**
 * @brief Obtiene el tiempo actual validado (evita tiempos futuros)
 * 
 * @return Tiempo validado en segundos Unix
 */
time_t time_get_validated_current(void) {
    time_t estimated = time_get_current();
    
    // Si el tiempo estimado es inválido (futuro), usar tiempo de sistema
    if (!time_is_timestamp_valid(estimated)) {
        ESP_LOGW(TAG, "Tiempo estimado invalido (%ld), usando tiempo de sistema", 
                 (long)estimated);
        return time(NULL);
    }
    
    return estimated;
}