idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_usb_serial_jtag i2s_spk esp_timer
)
