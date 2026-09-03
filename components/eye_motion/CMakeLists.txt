idf_component_register(
    SRCS "eye_motion.c"
    INCLUDE_DIRS "include"
    REQUIRES eye_servo esp_timer
)
