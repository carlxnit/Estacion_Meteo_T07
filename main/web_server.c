#include "system_config.h"

static const char *TAG = "WEB_SERVER";
static httpd_handle_t web_server = NULL;

// =============================================================================
// FUNCIONES AUXILIARES PRIVADAS
// =============================================================================

/**
 * @brief Decodifica strings codificados en URL
 */
static void url_decode(const char* src, char* dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) return;
    
    size_t i = 0, j = 0;
    
    while (src[i] != '\0' && j < dst_size - 1) {
        if (src[i] == '+') {
            dst[j++] = ' ';
        } else if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = {src[i+1], src[i+2], '\0'};
            char *endptr;
            long int val = strtol(hex, &endptr, 16);
            
            if (*endptr == '\0') {
                dst[j++] = (char)val;
                i += 2;
            } else {
                dst[j++] = src[i];
            }
        } else {
            dst[j++] = src[i];
        }
        i++;
    }
    dst[j] = '\0';
}

/**
 * @brief Parsea datos application/x-www-form-urlencoded
 */
static bool parse_form_data(const char* data, size_t data_len, 
                           char* ssid, size_t ssid_size,
                           char* password, size_t pass_size) {
    if (!data || data_len == 0) return false;
    
    char *data_copy = malloc(data_len + 1);
    if (!data_copy) return false;
    
    memcpy(data_copy, data, data_len);
    data_copy[data_len] = '\0';
    
    ssid[0] = '\0';
    password[0] = '\0';
    
    char *token = strtok(data_copy, "&");
    while (token != NULL) {
        char *equals = strchr(token, '=');
        if (equals) {
            *equals = '\0';
            char *key = token;
            char *value = equals + 1;
            
            if (strcmp(key, "ssid") == 0) {
                url_decode(value, ssid, ssid_size);
            } else if (strcmp(key, "password") == 0) {
                url_decode(value, password, pass_size);
            }
        }
        token = strtok(NULL, "&");
    }
    
    free(data_copy);
    return (strlen(ssid) > 0);
}

/**
 * @brief Guarda credenciales WiFi en NVS
 */
static bool save_wifi_credentials(const char* ssid, const char* password) {
    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID vacio, no se guarda");
        return false;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_config", NVS_READWRITE, &nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS: %s", esp_err_to_name(err));
        return false;
    }
    
    err = nvs_set_str(nvs_handle, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, "password", password);
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error guardando en NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error en commit NVS: %s", esp_err_to_name(err));
        return false;
    }
    
    ESP_LOGI(TAG, "Credenciales guardadas: SSID='%s'", ssid);
    return true;
}

/**
 * @brief Carga credenciales WiFi de NVS
 */
static bool load_wifi_credentials(char* ssid, size_t ssid_size, 
                                 char* password, size_t pass_size) {
    if (!ssid || !password) return false;
    
    ssid[0] = '\0';
    password[0] = '\0';
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_config", NVS_READONLY, &nvs_handle);
    
    if (err != ESP_OK) {
        return false;
    }
    
    size_t required_size = ssid_size;
    err = nvs_get_str(nvs_handle, "ssid", ssid, &required_size);
    
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return false;
    }
    
    required_size = pass_size;
    err = nvs_get_str(nvs_handle, "password", password, &required_size);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Password no configurado (red abierta)
        password[0] = '\0';
    } else if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return false;
    }
    
    nvs_close(nvs_handle);
    return true;
}

/**
 * @brief Borra credenciales WiFi de NVS
 */
static bool delete_wifi_credentials(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_config", NVS_READWRITE, &nvs_handle);
    
    if (err != ESP_OK) {
        return false;
    }
    
    err = nvs_erase_all(nvs_handle);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return false;
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (err != ESP_OK) {
        return false;
    }
    
    ESP_LOGI(TAG, "Credenciales borradas de NVS");
    return true;
}

/**
 * @brief Genera HTML para mostrar credenciales actuales
 */
static void generate_credentials_html(char* buffer, size_t buffer_size, 
                                     const char* current_ssid, 
                                     const char* current_password) {
    if (strlen(current_ssid) == 0) {
        // No hay credenciales guardadas
        snprintf(buffer, buffer_size,
            "<div style='background:#f8d7da;padding:10px;margin:10px 0;border-radius:5px;'>"
            "<strong>NO hay credenciales guardadas</strong><br>"
            "Configura una red WiFi para conectar"
            "</div>");
    } else {
        // Mostrar credenciales actuales
        const char* pass_display = strlen(current_password) > 0 ? 
            "********" : "(red abierta)";
        
        snprintf(buffer, buffer_size,
            "<div style='background:#d4edda;padding:10px;margin:10px 0;border-radius:5px;'>"
            "<strong>Credenciales actuales:</strong><br>"
            "SSID: <strong>%s</strong><br>"
            "Contrasena: %s<br>"
            "<form action='/delete' method='post' style='display:inline;margin-top:5px;'>"
            "<input type='submit' value='Borrar' style='background:#dc3545;color:white;border:none;padding:5px 10px;border-radius:3px;cursor:pointer;'>"
            "</form>"
            "</div>",
            current_ssid, pass_display);
    }
}

// =============================================================================
// HANDLERS HTTP
// =============================================================================

/**
 * @brief Handler para la pagina principal (/)
 */
static esp_err_t root_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Peticion a pagina principal desde IP");
    
    // Cargar credenciales actuales
    char current_ssid[33] = {0};
    char current_password[65] = {0};
    bool has_credentials = load_wifi_credentials(current_ssid, sizeof(current_ssid), 
                                                 current_password, sizeof(current_password));
    
    if (!has_credentials) {
        current_ssid[0] = '\0';
        current_password[0] = '\0';
    }
    
    // Generar HTML para credenciales
    char credentials_html[512];
    generate_credentials_html(credentials_html, sizeof(credentials_html), 
                             current_ssid, current_password);
    
    // HTML MINIMALISTA con credenciales
    const char* minimal_html_template = 
        "<html>"
        "<head>"
        "<title>ESP32 Config WiFi</title>"
        "<style>"
        "body { font-family: Arial, sans-serif; max-width: 400px; margin: 20px auto; padding: 20px; }"
        "h1 { color: #333; }"
        "form { background: #f9f9f9; padding: 15px; border-radius: 5px; }"
        "input[type=text], input[type=password] { width: 95%%; padding: 8px; margin: 5px 0; }"
        "input[type=submit] { background: #007bff; color: white; border: none; padding: 10px 15px; border-radius: 3px; cursor: pointer; }"
        "input[type=submit]:hover { background: #0056b3; }"
        "</style>"
        "</head>"
        "<body>"
        "<h1>ESP32 WiFi Config</h1>"
        "<p><strong>AP:</strong> %s</p>"
        "%s"  // Credenciales actuales
        "<h2>Configurar Nueva Red WiFi</h2>"
        "<form action='/save' method='post'>"
        "SSID:<br>"
        "<input type='text' name='ssid' placeholder='Nombre de la red'><br>"
        "Contrasena:<br>"
        "<input type='password' name='password' placeholder='Contrasena (opcional)'><br>"
        "<input type='submit' value='Guardar'>"
        "</form>"
        "</body>"
        "</html>";
    
    // Crear HTML final
    char final_html[2048];
    snprintf(final_html, sizeof(final_html), minimal_html_template, 
             wifi_get_ap_ssid(), credentials_html);
    
    ESP_LOGI(TAG, "Enviando respuesta HTML (%d bytes)", strlen(final_html));
    httpd_resp_set_type(req, "text/html");
    esp_err_t ret = httpd_resp_send(req, final_html, strlen(final_html));
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error enviando respuesta: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Respuesta enviada OK");
    }
    
    return ret;
}

/**
 * @brief Handler para guardar configuracion (/save)
 */
static esp_err_t save_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Recibiendo configuracion WiFi...");
    
    // Validar metodo
    if (req->method != HTTP_POST) {
        ESP_LOGE(TAG, "Metodo no permitido: %d (esperaba POST)", req->method);
        httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Metodo no permitido");
        return ESP_FAIL;
    }
    
    // Validar contenido
    if (req->content_len <= 0) {
        ESP_LOGE(TAG, "Contenido vacio o sin Content-Length");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Sin datos en el formulario");
        return ESP_FAIL;
    }
    
    // Limitar tamaño maximo
    if (req->content_len > 2048) {
        ESP_LOGE(TAG, "Datos POST demasiado grandes: %d bytes (max: 2048)", req->content_len);
        httpd_resp_send_err(req, HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE, "Datos demasiado grandes");
        return ESP_FAIL;
    }
    
    char ssid[33] = {0};
    char password[65] = {0};
    
    // Buffer dinamico para los datos POST
    char* content = malloc(req->content_len + 1);
    if (!content) {
        ESP_LOGE(TAG, "Error de memoria al allocar %d bytes", req->content_len + 1);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error interno del servidor");
        return ESP_FAIL;
    }
    
    // Leer todos los datos POST
    int total_received = 0;
    while (total_received < req->content_len) {
        int ret = httpd_req_recv(req, content + total_received, req->content_len - total_received);
        
        if (ret < 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "Timeout recibiendo datos POST");
                free(content);
                httpd_resp_send_408(req);
                return ESP_FAIL;
            }
            ESP_LOGE(TAG, "Error recibiendo datos POST: %d", ret);
            free(content);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error recibiendo datos");
            return ESP_FAIL;
        }
        
        if (ret == 0) {
            // Conexion cerrada por cliente
            ESP_LOGE(TAG, "Conexion cerrada por cliente durante recepcion");
            free(content);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Conexion interrumpida");
            return ESP_FAIL;
        }
        
        total_received += ret;
        ESP_LOGD(TAG, "Recibidos %d bytes (total: %d/%d)", ret, total_received, req->content_len);
    }
    
    // Terminar string
    content[total_received] = '\0';
    ESP_LOGD(TAG, "Datos POST completos (%d bytes): %s", total_received, content);
    
    // Parsear datos del formulario
    if (!parse_form_data(content, total_received, ssid, sizeof(ssid), password, sizeof(password))) {
        ESP_LOGE(TAG, "Error parseando datos del formulario");
        free(content);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Datos del formulario invalidos");
        return ESP_FAIL;
    }
    
    free(content);
    
    // Validar SSID
    if (strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID vacio");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID no puede estar vacio");
        return ESP_FAIL;
    }
    
    if (strlen(ssid) > 32) {
        ESP_LOGE(TAG, "SSID demasiado largo: %d caracteres", strlen(ssid));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID demasiado largo (max 32 chars)");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Configuracion recibida - SSID: '%s', Password: '%s'", 
             ssid, strlen(password) > 0 ? "[PROTEGIDA]" : "(sin contrasena)");
    
    // Guardar en NVS
    if (!save_wifi_credentials(ssid, password)) {
        ESP_LOGE(TAG, "Error guardando credenciales en NVS");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error guardando configuracion");
        return ESP_FAIL;
    }
    
    // Pagina con comprobacion en vivo: muestra un banner cuando el ESP32
    // confirma la conexion a la nueva red (sin perder el portal mientras prueba)
    char response[2048];
    snprintf(response, sizeof(response),
             "<!DOCTYPE html>"
             "<html>"
             "<head>"
             "<meta charset='UTF-8'>"
             "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
             "<title>Conectando...</title>"
             "<style>"
             "body { font-family: Arial, sans-serif; text-align: center; padding: 50px; }"
             "#banner { display:none; font-size: 20px; padding: 15px; border-radius: 5px; margin-top: 20px; }"
             ".ok { background:#d4edda; color:#155724; }"
             ".err { background:#f8d7da; color:#721c24; }"
             "</style>"
             "</head>"
             "<body>"
             "<h1>Probando conexión a '%s'...</h1>"
             "<p id='spinner'>Esperando confirmación del ESP32...</p>"
             "<div id='banner'></div>"
             "<script>"
             "let tries = 0;"
             "const ssid = '%s';"
             "function poll() {"
             "  tries++;"
             "  fetch('/status').then(r => r.json()).then(d => {"
             "    if (d.connected) {"
             "      document.getElementById('spinner').style.display='none';"
             "      const b = document.getElementById('banner');"
             "      b.className = 'ok'; b.style.display = 'block';"
             "      b.innerHTML = '✅ Conectado correctamente a la red \\'' + ssid + '\\' (IP: ' + d.ip + '). Reiniciando...';"
             "      fetch('/restart', {method:'POST'});"
             "    } else if (tries >= 20) {"
             "      document.getElementById('spinner').style.display='none';"
             "      const b = document.getElementById('banner');"
             "      b.className = 'err'; b.style.display = 'block';"
             "      b.innerHTML = '❌ No se pudo conectar a \\'' + ssid + '\\'. Verifica la contraseña e inténtalo de nuevo desde <a href=\"/\">aquí</a>.';"
             "    } else {"
             "      setTimeout(poll, 1000);"
             "    }"
             "  }).catch(() => setTimeout(poll, 1000));"
             "}"
             "setTimeout(poll, 1500);"
             "</script>"
             "</body>"
             "</html>",
             ssid, ssid);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));

    ESP_LOGI(TAG, "Credenciales guardadas. Probando conexión sin perder el portal...");
    wifi_try_connect_async(ssid, password);

    return ESP_OK;
}

/**
 * @brief Handler de estado de conexión, consultado por la página de /save (/status)
 */
static esp_err_t status_handler(httpd_req_t *req) {
    bool connected = wifi_is_connected();
    char ip[32] = "";
    if (connected) {
        wifi_get_sta_ip(ip, sizeof(ip));
    }

    char response[160];
    snprintf(response, sizeof(response),
             "{\"connected\":%s,\"ip\":\"%s\"}",
             connected ? "true" : "false", ip);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/**
 * @brief Handler para reiniciar el ESP32 una vez confirmada la nueva conexión (/restart)
 */
static esp_err_t restart_handler(httpd_req_t *req) {
    if (req->method != HTTP_POST) {
        httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Metodo no permitido");
        return ESP_FAIL;
    }

    const char *response = "{\"ok\":true}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));

    ESP_LOGI(TAG, "Reinicio solicitado desde el portal tras conexión confirmada...");
    vTaskDelay(500 / portTICK_PERIOD_MS);
    esp_restart();

    return ESP_OK;
}

/**
 * @brief Handler para borrar credenciales (/delete)
 */
static esp_err_t delete_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Recibiendo peticion para borrar credenciales...");
    
    // Validar metodo
    if (req->method != HTTP_POST) {
        ESP_LOGE(TAG, "Metodo no permitido: %d (esperaba POST)", req->method);
        httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Metodo no permitido");
        return ESP_FAIL;
    }
    
    // Borrar credenciales
    if (!delete_wifi_credentials()) {
        ESP_LOGE(TAG, "Error borrando credenciales de NVS");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error borrando configuracion");
        return ESP_FAIL;
    }
    
    // Pagina de confirmacion con redireccion
    char response[768];
    snprintf(response, sizeof(response), 
             "<!DOCTYPE html>"
             "<html>"
             "<head>"
             "<meta charset='UTF-8'>"
             "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
             "<meta http-equiv='refresh' content='3;url=/'>"
             "<title>Credenciales Borradas</title>"
             "<style>"
             "body { font-family: Arial, sans-serif; text-align: center; padding: 50px; }"
             ".warning { color: #856404; font-size: 24px; }"
             ".info { background: #fff3cd; padding: 15px; margin: 20px; border-radius: 5px; }"
             "</style>"
             "</head>"
             "<body>"
             "<h1 class='warning'>Credenciales Borradas</h1>"
             "<div class='info'>"
             "<p>Las credenciales WiFi han sido eliminadas.</p>"
             "<p>El dispositivo volvera a modo Access Point.</p>"
             "</div>"
             "<p>Redirigiendo a la pagina principal en 3 segundos...</p>"
             "</body>"
             "</html>");
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));
    
    ESP_LOGI(TAG, "Credenciales borradas. Reiniciando...");
    
    // Pausa para que se envie la respuesta
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    
    // Programar reinicio
    ESP_LOGI(TAG, "Programando reinicio en 2 segundos...");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "Reiniciando para volver a modo Access Point...");
    esp_restart();
    
    return ESP_OK;
}

// =============================================================================
// FUNCIONES PUBLICAS
// =============================================================================

void web_server_start(void) {
    if (web_server != NULL) {
        ESP_LOGW(TAG, "Servidor web ya iniciado");
        return;
    }
    
    ESP_LOGI(TAG, "Iniciando servidor web de configuracion...");
    
    // Configuracion del servidor HTTP
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.stack_size = 12288;
    config.max_uri_handlers = 15;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    
    // Iniciar servidor
    esp_err_t ret = httpd_start(&web_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando servidor web: %s", esp_err_to_name(ret));
        return;
    }
    
    // Registrar handlers
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL
    };
    
    httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_handler,
        .user_ctx = NULL
    };
    
    httpd_uri_t delete_uri = {
        .uri = "/delete",
        .method = HTTP_POST,
        .handler = delete_handler,
        .user_ctx = NULL
    };

    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL
    };

    httpd_uri_t restart_uri = {
        .uri = "/restart",
        .method = HTTP_POST,
        .handler = restart_handler,
        .user_ctx = NULL
    };

    // Registrar todos los handlers
    httpd_register_uri_handler(web_server, &root_uri);
    httpd_register_uri_handler(web_server, &save_uri);
    httpd_register_uri_handler(web_server, &delete_uri);
    httpd_register_uri_handler(web_server, &status_uri);
    httpd_register_uri_handler(web_server, &restart_uri);
    
    ESP_LOGI(TAG, "Servidor web iniciado correctamente");
    ESP_LOGI(TAG, "URL: http://192.168.4.1");
}

void web_server_stop(void) {
    if (web_server != NULL) {
        ESP_LOGI(TAG, "Deteniendo servidor web...");
        httpd_stop(web_server);
        web_server = NULL;
        ESP_LOGI(TAG, "Servidor web detenido");
    }
}

bool web_server_is_running(void) {
    return (web_server != NULL);
}

const char* web_server_get_ip(void) {
    return "192.168.4.1";
}