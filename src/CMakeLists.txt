idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board pca9685 eye_servo eye_motion eye_vision eye_web eye_console eye_net nvs_flash esp_driver_gpio
)
