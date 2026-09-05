idf_component_register(
    SRCS "eye_console.c"
    INCLUDE_DIRS "include"
    REQUIRES board eye_servo eye_motion eye_vision
)
