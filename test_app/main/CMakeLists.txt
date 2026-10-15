idf_component_register(
    SRCS "test_app_main.c" "test_i2s_spk.c"
    INCLUDE_DIRS "."
    REQUIRES unity i2s_spk esp_partition esp_timer
    WHOLE_ARCHIVE
)
