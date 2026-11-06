idf_component_register(
    SRCS "main.c"
         "test_ui.c"
         "whale_link_diag.c"
         "whale_ble.c"
         "whale_file.c"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash esp_timer
             bt spi_flash
)

# 本工程不编译产品侧的 whale_ui / whale_model / whale_audio / whale_store，
# 也不使用 assets 里的中文字模与角色图 —— 界面走 LVGL 自带 Montserrat，
# 与产品固件在界面、字库、图片上完全没有交集。
