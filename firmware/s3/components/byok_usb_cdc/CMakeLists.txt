# SPDX-License-Identifier: MIT
# byok_usb_cdc -- TinyUSB CDC-ACM transport, on top of the managed component
# espressif/esp_tinyusb (declared in main/idf_component.yml; the component
# manager makes it available under the name "esp_tinyusb" to any component
# in the build that REQUIRES it, regardless of which component's
# idf_component.yml first pulled it in).
idf_component_register(
    SRCS "byok_usb_cdc.c"
    INCLUDE_DIRS "include"
    # "hal"/"soc" are for the USB-Serial-JTAG / USB_WRAP low-level PHY-mux
    # registers byok_usb_cdc_restore_usj_phy() writes (see byok_usb_cdc.c);
    # "esp_system" is for esp_register_shutdown_handler()/esp_reset_reason().
    # "driver" is for i2c_master_dev_handle_t/i2c_master_bus_add_device/
    # i2c_master_transmit_receive (TUSB320 reads); "byok_sysbus" is the
    # shared I2C BUS 1 handle those go through; "byok_hw_shim" is the
    # BYOK_TUSB320_* register/address constants (2026-09-03, USB re-attach
    # watcher -- see byok_usb_cdc.c's own module comment on that task).
    PRIV_REQUIRES "esp_tinyusb" "log" "freertos" "esp_common" "esp_hw_support" "mbedtls"
                  "hal" "soc" "esp_system" "driver" "byok_sysbus" "byok_hw_shim"
)
