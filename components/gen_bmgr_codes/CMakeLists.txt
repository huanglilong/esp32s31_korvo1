# Board information output
message(STATUS "Selected Board: esp32_s31_korvo1")
message(STATUS "Board Path: ${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__esp_boards/esp32_s31_korvo_1")

idf_component_register(
    SRCS
        "gen_board_info.c"
        "gen_board_periph_config.c"
        "gen_board_periph_handles.c"
        "gen_board_device_config.c"
        "gen_board_device_handles.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__esp_boards/esp32_s31_korvo_1/setup_device.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    REQUIRES esp_board_manager esp_codec_dev esp_lcd_touch button led_strip esp_video
)

# Force all symbols from gen_bmgr_codes to be linked (board manager references them)
idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
