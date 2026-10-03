idf_component_register(SRCS "app_main.c" "bridge.c" "radio.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer
                                     link_proto mav_framer pairing health serial_port)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wshadow)
