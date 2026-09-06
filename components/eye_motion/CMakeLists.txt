idf_component_register(
    SRCS "eye_motion.c"
    INCLUDE_DIRS "include"
    REQUIRES eye_servo eye_vision esp_timer nvs_flash
)
