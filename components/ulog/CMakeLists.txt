idf_component_register(
    SRCS "ulog_writer.cpp"
    INCLUDE_DIRS "include"
    REQUIRES "uorb" "freertos" "esp_timer"
)
