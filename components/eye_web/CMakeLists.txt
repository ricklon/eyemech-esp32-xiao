# The control page is linked into the firmware as a C array.
#
# ESP-IDF's EMBED_TXTFILES would normally do this, but PlatformIO's builder
# intercepts it: for the espidf framework it generates the .S and then only
# calls env.Requires() on it, never adding it to the link, so
# _binary_index_html_start is never defined. Generating the array at CMake
# configure time sidesteps that entirely — no build-time tooling, no new
# dependency, and editing index.html re-runs configure.
set(index_html   "${CMAKE_CURRENT_LIST_DIR}/index.html")
set(index_html_c "${CMAKE_CURRENT_BINARY_DIR}/index_html.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${index_html}")

file(READ "${index_html}" index_html_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," index_html_bytes "${index_html_hex}")
file(WRITE "${index_html_c}"
     "/* Generated from index.html at configure time by CMakeLists.txt. Do not edit. */\n"
     "const unsigned char index_html_start[] = {${index_html_bytes}0x00};\n"
     "const unsigned int  index_html_len = sizeof(index_html_start) - 1;\n")

idf_component_register(
    SRCS "eye_web.c" "eye_mcp.c" "${index_html_c}"
    INCLUDE_DIRS "include"
    REQUIRES board eye_servo eye_motion eye_vision eye_net esp_http_server espressif__cjson
)
