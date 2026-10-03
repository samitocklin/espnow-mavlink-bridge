idf_component_register(SRCS "serial_port.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_uart esp_driver_usb_serial_jtag health)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wshadow)
