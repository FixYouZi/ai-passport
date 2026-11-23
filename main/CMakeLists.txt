idf_component_register(
    SRCS "main.c"
         "tesla_proto.c"
         "tesla_crypto.c"
         "tesla_config.c"
         "tesla_identity.c"
         "tesla_identity_esp.c"
         "tesla_ble.c"
         "tesla_web.c"
         "tesla_app.c"
         "car_art.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_http_server esp_hw_support esp_netif esp_timer esp_wifi mbedtls nvs_flash
)

# 中文位图字体由 lv_font_conv 生成，素材与许可证见 assets/README.md。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/tesla_font_lock.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/tesla_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/tesla_font_20.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/tesla_font_26.c"
)
