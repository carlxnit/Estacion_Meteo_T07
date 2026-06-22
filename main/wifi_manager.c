#include "system_config.h"

static const char *TAG = "WIFI_MANAGER";

static EventGroupHandle_t s_wifi_event_group;
static int wifi_conectado = 0;
static bool credentials_failed = false;
static char ap_ssid[32];
static esp_netif_t *s_sta_netif = NULL;
static int s_retry_num = 0;
static bool s_testing_new_credentials = false;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// =============================================================================
// FUNCIONES PRIVADAS
// =============================================================================
// Función para leer credenciales WiFi de NVS
static bool load_wifi_credentials(char* ssid, size_t ssid_size, char* password, size_t pass_size) {
    if (!ssid || !password || ssid_size == 0 || pass_size == 0) {
        return false;
    }
    
    // Inicializar buffers
    ssid[0] = '\0';
    password[0] = '\0';
    
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    // Abrir namespace NVS
    err = nvs_open("wifi_config", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "Namespace 'wifi_config' no encontrado en NVS");
        } else {
            ESP_LOGE(TAG, "Error abriendo NVS: %s", esp_err_to_name(err));
        }
        return false;
    }
    
    // Leer SSID
    size_t required_size = ssid_size;
    err = nvs_get_str(nvs_handle, "ssid", ssid, &required_size);
    
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "SSID no encontrado en NVS");
        } else {
            ESP_LOGE(TAG, "Error leyendo SSID de NVS: %s", esp_err_to_name(err));
        }
        nvs_close(nvs_handle);
        return false;
    }
    
    // Validar SSID
    if (strlen(ssid) == 0 || strlen(ssid) > 32) {
        ESP_LOGW(TAG, "SSID en NVS inválido (longitud: %d)", strlen(ssid));
        nvs_close(nvs_handle);
        return false;
    }
    
    // Leer password
    required_size = pass_size;
    err = nvs_get_str(nvs_handle, "password", password, &required_size);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Password no configurado (red abierta) - esto es OK
        password[0] = '\0';
        ESP_LOGD(TAG, "Password no configurado en NVS (red abierta)");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error leyendo password de NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    // Validar password (puede estar vacío para redes abiertas)
    if (strlen(password) > 64) {
        ESP_LOGW(TAG, "Password en NVS demasiado largo (%d chars)", strlen(password));
        nvs_close(nvs_handle);
        return false;
    }
    
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "📂 Credenciales cargadas de NVS exitosamente");
    return true;
}

/**
 * @brief Genera SSID único para el AP
 */
static void generate_ap_ssid(void) {
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(ap_ssid, sizeof(ap_ssid), "ESP32_%02X%02X", mac[4], mac[5]);
    ESP_LOGI(TAG, "SSID del AP: %s", ap_ssid);
}

/**
 * @brief Manejador de eventos WiFi
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "📡 WiFi iniciado - Conectando...");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_conectado = 0;

        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGE(TAG, "❌ WiFi desconectado. Razón: %d", disconnected->reason);

        // Detectar fallo de credenciales
        if (disconnected->reason == WIFI_REASON_AUTH_EXPIRE ||
            disconnected->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            disconnected->reason == WIFI_REASON_AUTH_FAIL) {
            ESP_LOGW(TAG, "🔐 Credenciales incorrectas!");
            credentials_failed = true;
        }

        if (s_retry_num < MAX_INTENTOS && !credentials_failed) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "🔄 Reintento WiFi %d/%d", s_retry_num, MAX_INTENTOS);
        } else {
            ESP_LOGE(TAG, "⏹️ Fallo permanente WiFi");
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_conectado = 1;
        credentials_failed = false;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "✅ WiFi conectado! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;

        // Detener servidor web salvo que estemos probando credenciales nuevas
        // desde el portal: ahí lo necesitamos vivo para servir /status y /restart
        if (!s_testing_new_credentials && web_server_is_running()) {
            web_server_stop();
        }

        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "📱 Dispositivo conectado al AP: " MACSTR, MAC2STR(event->mac));
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "📱 Dispositivo desconectado del AP: " MACSTR, MAC2STR(event->mac));
    }
}

/**
 * @brief Inicia modo Access Point
 */
static void wifi_start_ap(void) {
    ESP_LOGI(TAG, "🌐 Iniciando Access Point...");
    
    // Detener WiFi actual
    esp_wifi_stop();
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    // Configurar modo AP
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando modo AP: %s", esp_err_to_name(err));
        return;
    }
    
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "",
            .ssid_len = 0,
            .channel = 1,
            .password = AP_PASSWORD,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {
                .required = true,
            },
        },
    };
    
    strncpy((char*)wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(ap_ssid);
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Iniciar servidor web de configuración
    web_server_start();
    
    ESP_LOGI(TAG, "✅ AP iniciado: %s", ap_ssid);
    ESP_LOGI(TAG, "🔑 Contraseña: %s", AP_PASSWORD);
    ESP_LOGI(TAG, "💻 Configura en: http://%s", web_server_get_ip());
}

/**
 * @brief Conecta a red WiFi (STA)
 */
static void wifi_connect_sta(void) {
    char ssid[33] = {0};
    char password[65] = {0};
    bool using_nvs = false;
    
    ESP_LOGI(TAG, "=== INICIANDO CONEXIÓN WiFi ===");
    
    // 1. PRIMERO intentar cargar credenciales de NVS (lo que guardaste por AP)
    if (load_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        if (strlen(ssid) > 0) {
            using_nvs = true;
            ESP_LOGI(TAG, "✅ Usando credenciales de NVS:");
            ESP_LOGI(TAG, "   SSID: '%s'", ssid);
            ESP_LOGI(TAG, "   Password: %s", 
                     strlen(password) > 0 ? "[PROTEGIDA]" : "(red abierta)");
        } else {
            ESP_LOGW(TAG, "NVS tiene SSID vacío, probando hardcodeadas...");
        }
    }
    
    // 2. Si NO hay credenciales en NVS válidas, usar hardcodeadas
    if (!using_nvs && strlen(WIFI_SSID) > 0) {
        strncpy(ssid, WIFI_SSID, sizeof(ssid) - 1);
        strncpy(password, WIFI_PASS, sizeof(password) - 1);
        ESP_LOGI(TAG, "⚠️ Usando credenciales HARCODEADAS:");
        ESP_LOGI(TAG, "   SSID: '%s'", ssid);
        ESP_LOGI(TAG, "   Password: %s", 
                 strlen(password) > 0 ? "[PROTEGIDA]" : "(red abierta)");
    }
    
    // 3. Si no hay credenciales en absoluto, ir a modo AP
    if (strlen(ssid) == 0) {
        ESP_LOGW(TAG, "❌ No hay credenciales WiFi (ni NVS ni hardcodeadas)");
        ESP_LOGW(TAG, "🚀 Iniciando modo Access Point...");
        wifi_start_ap();
        return;
    }
    
    // 4. Configurar conexión WiFi
    ESP_LOGI(TAG, "🔧 Configurando WiFi STA...");
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    // Detener WiFi primero si estaba activo
    esp_wifi_stop();
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    wifi_config_t wifi_config = {0};
    
    // Copiar SSID
    size_t ssid_len = strlen(ssid);
    if (ssid_len > 32) ssid_len = 32;
    memcpy(wifi_config.sta.ssid, ssid, ssid_len);
    
    // Copiar password
    size_t pass_len = strlen(password);
    if (pass_len > 64) pass_len = 64;
    memcpy(wifi_config.sta.password, password, pass_len);
    
    // Configurar auth mode
    if (strlen(password) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        ESP_LOGI(TAG, "🔓 Modo: Red ABIERTA (sin contraseña)");
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_LOGI(TAG, "🔐 Modo: WPA2-PSK");
    }
    
    // Configuración adicional para mejor conexión
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;           // Escaneo rápido
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL; // Conectar por mejor señal
    wifi_config.sta.threshold.rssi = -127;                  // Cualquier señal
    wifi_config.sta.pmf_cfg.capable = true;                 // PMF capaz
    wifi_config.sta.pmf_cfg.required = false;               // PMF no requerido
    
    // Aplicar configuración
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // Configurar protocolo (solo 2.4GHz)
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, 
                                          WIFI_PROTOCOL_11B | 
                                          WIFI_PROTOCOL_11G | 
                                          WIFI_PROTOCOL_11N));
    
    // Deshabilitar power saving durante conexión
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    
    // Iniciar WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Pequeña pausa para estabilización
    vTaskDelay(200 / portTICK_PERIOD_MS);
    
    // Intentar conexión
    ESP_LOGI(TAG, "🔄 Intentando conectar a: '%s'", ssid);
    esp_err_t connect_ret = esp_wifi_connect();
    
    if (connect_ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Error en esp_wifi_connect(): %s", esp_err_to_name(connect_ret));
        
        // Si falla inmediatamente y estamos usando NVS, mostrar más info
        if (using_nvs) {
            ESP_LOGI(TAG, "⚠️  Fallo con credenciales de NVS. Verifica:");
            ESP_LOGI(TAG, "   1. Red '%s' existe y está en rango", ssid);
            ESP_LOGI(TAG, "   2. Contraseña es correcta");
            ESP_LOGI(TAG, "   3. Red es 2.4GHz (ESP32 no soporta 5GHz)");
        }
    } else {
        ESP_LOGI(TAG, "✅ Llamada a esp_wifi_connect() exitosa");
        ESP_LOGI(TAG, "⏳ Esperando resultado de conexión...");
    }
}

// =============================================================================
// FUNCIÓN PÚBLICA PRINCIPAL
// =============================================================================

void wifi_init_sta(void) {
    ESP_LOGI(TAG, "🚀 Inicializando WiFi Manager...");
    
    s_wifi_event_group = xEventGroupCreate();
    
    // Inicializar stack de red
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Crear interfaces
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    assert(sta_netif && ap_netif);
    s_sta_netif = sta_netif;
    
    // Configurar WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Registrar manejadores
    esp_event_handler_instance_t instance_any_id, instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &instance_got_ip));
    
    // Generar SSID del AP
    generate_ap_ssid();
    
    // Intentar conexión STA primero
    wifi_connect_sta();
    
    ESP_LOGI(TAG, "⏳ Esperando conexión WiFi (30 segundos)...");
    
    // Esperar conexión con timeout
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    // Los manejadores de eventos quedan registrados permanentemente: se
    // necesitan despues para detectar la conexion al probar credenciales
    // nuevas desde el portal de configuracion (ver wifi_try_connect_async)
    (void)instance_got_ip;
    (void)instance_any_id;

    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    
    // Decidir modo de operación
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "✅ WiFi CONECTADO! Modo STA activo");
    } else if (bits & WIFI_FAIL_BIT || credentials_failed) {
        ESP_LOGW(TAG, "🔄 Iniciando modo Access Point...");
        if (credentials_failed) {
            ESP_LOGE(TAG, "   Razón: Credenciales incorrectas");
        } else {
            ESP_LOGE(TAG, "   Razón: Timeout de conexión");
        }
        wifi_start_ap();
    } else {
        ESP_LOGE(TAG, "⏰ TIMEOUT WiFi - Iniciando modo AP");
        wifi_start_ap();
    }
}

// =============================================================================
// FUNCIONES PÚBLICAS
// =============================================================================

bool wifi_is_connected(void) {
    return wifi_conectado;
}

bool wifi_is_ap_mode(void) {
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    return (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
}

const char* wifi_get_ap_ssid(void) {
    return ap_ssid;
}

bool wifi_get_sta_ip(char *buf, size_t len) {
    if (!s_sta_netif || !buf || len == 0) {
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }

    snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

/**
 * @brief Prueba credenciales nuevas en modo APSTA sin tirar el portal de
 *        configuracion, para poder confirmar la conexion antes de reiniciar.
 */
void wifi_try_connect_async(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) {
        return;
    }

    ESP_LOGI(TAG, "🔄 Probando nuevas credenciales WiFi: '%s'", ssid);
    s_testing_new_credentials = true;
    credentials_failed = false;
    s_retry_num = 0;
    wifi_conectado = 0;

    esp_wifi_stop();
    vTaskDelay(100 / portTICK_PERIOD_MS);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Reconfigurar el AP para no perder el portal mientras probamos la STA
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "",
            .ssid_len = 0,
            .channel = 1,
            .password = AP_PASSWORD,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {
                .required = true,
            },
        },
    };
    strncpy((char*)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    wifi_config_t sta_config = {0};
    size_t ssid_len = strlen(ssid);
    if (ssid_len > 32) ssid_len = 32;
    memcpy(sta_config.sta.ssid, ssid, ssid_len);

    size_t pass_len = strlen(password ? password : "");
    if (pass_len > 64) pass_len = 64;
    if (password) memcpy(sta_config.sta.password, password, pass_len);

    sta_config.sta.threshold.authmode = (pass_len == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    sta_config.sta.scan_method = WIFI_FAST_SCAN;
    sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_config.sta.threshold.rssi = -127;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    ESP_ERROR_CHECK(esp_wifi_start());
    vTaskDelay(200 / portTICK_PERIOD_MS);

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Error en esp_wifi_connect() al probar nueva red: %s", esp_err_to_name(ret));
    }
}