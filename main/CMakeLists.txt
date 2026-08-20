idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        otool_dev_autodetect
        otool_tab5_component
        otool_lvgl_idf_port
        lvgl__lvgl
        nvs_flash
)
