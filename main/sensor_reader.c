#include "system_config.h"

static const char *TAG = "SENSOR_READER";

static adc_oneshot_unit_handle_t s_adc1_handle = NULL;

// ==================== FUNCIÓN COMÚN ADC ====================

static float read_adc_value(adc_channel_t channel, const char* sensor_name) {
    // Promediar lecturas para reducir ruido
    int total = 0;
    int raw = 0;
    for (int i = 0; i < 10; i++) {
        adc_oneshot_read(s_adc1_handle, channel, &raw);
        total += raw;
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    int raw_value = total / 10;

    // Convertir a voltaje (12 bits, 3.3V referencia)
    float voltage = (raw_value / 4095.0) * 3.3;

    ESP_LOGD(TAG, "%s - Raw: %d, Voltaje: %.3fV", sensor_name, raw_value, voltage);

    return voltage;
}

// ==================== LECTURA PLUVIÓMETRO ====================

float read_pluviometro_value(void) {
    float voltage = read_adc_value(PLUVIOMETRO_ADC_CHANNEL, "Pluviometro");

    // Convertir voltaje a mm de lluvia (lineal)
    float rainfall_mm = 0.0;

    if (voltage <= PLUVIOMETRO_MIN_VOLTAGE) {
        rainfall_mm = 0.0;
    } else if (voltage >= PLUVIOMETRO_MAX_VOLTAGE) {
        rainfall_mm = PLUVIOMETRO_MAX_MM;
    } else {
        // Interpolación lineal
        rainfall_mm = ((voltage - PLUVIOMETRO_MIN_VOLTAGE) /
                      (PLUVIOMETRO_MAX_VOLTAGE - PLUVIOMETRO_MIN_VOLTAGE)) * PLUVIOMETRO_MAX_MM;
    }

    ESP_LOGI(TAG, "Pluviometro - Voltaje: %.3fV, Lluvia: %.2f mm", voltage, rainfall_mm);
    return rainfall_mm;
}

// ==================== LECTURA ANEMÓMETRO ====================

float read_anemometro_value(void) {
    float voltage = read_adc_value(ANEMOMETRO_ADC_CHANNEL, "Anemometro");

    // Convertir voltaje a velocidad del viento (lineal)
    float wind_speed_ms = 0.0;

    if (voltage <= ANEMOMETRO_MIN_VOLTAGE) {
        wind_speed_ms = 0.0;
    } else if (voltage >= ANEMOMETRO_MAX_VOLTAGE) {
        wind_speed_ms = ANEMOMETRO_MAX_MS;
    } else {
        // Interpolación lineal
        wind_speed_ms = ((voltage - ANEMOMETRO_MIN_VOLTAGE) /
                        (ANEMOMETRO_MAX_VOLTAGE - ANEMOMETRO_MIN_VOLTAGE)) * ANEMOMETRO_MAX_MS;
    }

    float wind_speed_kmh = wind_speed_ms * 3.6;

    ESP_LOGI(TAG, "Anemometro - Voltaje: %.3fV, Velocidad: %.1f m/s (%.1f km/h)",
             voltage, wind_speed_ms, wind_speed_kmh);

    return wind_speed_ms;  // Retorna en m/s
}

// ==================== INICIALIZACIÓN ====================

void init_sensors(void) {
    ESP_LOGI(TAG, "Inicializando sensores meteorológicos ADC...");

    // Crear unidad ADC1
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &s_adc1_handle));

    // Configurar canales para ambos sensores
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };

    // Pluviómetro
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc1_handle, PLUVIOMETRO_ADC_CHANNEL, &chan_cfg));

    // Anemómetro
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc1_handle, ANEMOMETRO_ADC_CHANNEL, &chan_cfg));

    ESP_LOGI(TAG, "Sensores inicializados:");
    ESP_LOGI(TAG, "Pluviómetro en ADC_CHANNEL_%d (GPIO32)", PLUVIOMETRO_ADC_CHANNEL);
    ESP_LOGI(TAG, "Anemómetro en ADC_CHANNEL_%d (GPIO33)", ANEMOMETRO_ADC_CHANNEL);

    // Pequeña pausa para estabilización
    vTaskDelay(100 / portTICK_PERIOD_MS);
}
