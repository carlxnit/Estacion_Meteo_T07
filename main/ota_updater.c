#include "system_config.h"
#include <string.h>

static const char *TAG = "OTA_UPDATER";

// Descarga firmware/version.txt de GitHub y lo compara con FIRMWARE_VERSION.
// remote_version_out debe tener al menos 32 bytes.
bool ota_new_version_available(char *remote_version_out, size_t remote_version_size) {
    char buffer[64] = {0};

    esp_http_client_config_t config = {
        .url = GITHUB_VERSION_URL,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "❌ No se pudo crear cliente HTTP para versión");
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Error abriendo conexión de versión: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
    int status_code = esp_http_client_get_status_code(client);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status_code != 200 || read_len <= 0) {
        ESP_LOGW(TAG, "⚠️  No se pudo leer version.txt (HTTP %d)", status_code);
        return false;
    }

    // Quitar saltos de línea / espacios al final
    buffer[read_len] = '\0';
    for (int i = read_len - 1; i >= 0 && (buffer[i] == '\n' || buffer[i] == '\r' || buffer[i] == ' '); i--) {
        buffer[i] = '\0';
    }

    ESP_LOGI(TAG, "🔎 Versión actual: %s | Versión remota: %s", FIRMWARE_VERSION, buffer);

    if (remote_version_out != NULL) {
        strncpy(remote_version_out, buffer, remote_version_size - 1);
        remote_version_out[remote_version_size - 1] = '\0';
    }

    return strcmp(buffer, FIRMWARE_VERSION) != 0;
}

void check_ota_updates(void) {
    char remote_version[32] = {0};

    ESP_LOGI(TAG, "🔍 Comprobando actualizaciones OTA en GitHub...");

    if (!ota_new_version_available(remote_version, sizeof(remote_version))) {
        ESP_LOGI(TAG, "✅ Firmware ya está actualizado (v%s)", FIRMWARE_VERSION);
        return;
    }

    ESP_LOGI(TAG, "🆕 Nueva versión disponible (%s -> %s). Iniciando descarga...",
             FIRMWARE_VERSION, remote_version);

    esp_http_client_config_t config = {
        .url = GITHUB_FIRMWARE_URL,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "🎉 OTA EXITOSO! Reiniciando...");
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "❌ OTA falló: %s", esp_err_to_name(ret));
    }
}
