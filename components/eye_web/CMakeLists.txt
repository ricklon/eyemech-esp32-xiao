idf_component_register(
    SRCS "eye_web.c"
    INCLUDE_DIRS "include"
    REQUIRES board eye_servo eye_motion eye_vision esp_wifi esp_http_server esp_netif json
    EMBED_TXTFILES "index.html"
)
