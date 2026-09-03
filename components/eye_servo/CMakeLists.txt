idf_component_register(
    SRCS "eye_servo.c"
    INCLUDE_DIRS "include"
    REQUIRES pca9685 nvs_flash
)
