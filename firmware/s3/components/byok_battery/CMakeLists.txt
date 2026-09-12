# SPDX-License-Identifier: MIT
# byok_battery -- battery voltage + charger status (hw_config.h Sec.9, which
# carries the stock-image evidence for every constant used here). adc_oneshot + adc_cali (esp_adc) for the ADC front
# end, "driver" for GPIO12/13 (CHRG/STDBY).
idf_component_register(
    SRCS "byok_battery.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "byok_hw_shim" "esp_adc" "driver" "freertos" "log" "esp_common"
)
