idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES eye_servo eye_motion eye_web nvs_flash esp_wifi
)
