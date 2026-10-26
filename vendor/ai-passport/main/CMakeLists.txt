if(CONFIG_APP_ENABLE_SCREENSHOT)
    set(APP_SCREENSHOT_SRC "app_screenshot.c")
else()
    set(APP_SCREENSHOT_SRC "")
endif()

idf_component_register(
    SRCS "main.c"
         "app_proto.c"
         "app_link.c"
         "app_question_logic.c"
         "app_session_logic.c"
         "app_settings_logic.c"
         "app_settings.c"
         "app_pair_logic.c"
         "app_pair.c"
         "app_conn_logic.c"
         "app_ui.c"
         "app_screens.c"
         "app_alert.c"
         "app_voice.c"
         "app.c"
         "fonts/bitfont_8.c"
         "fonts/bitfont_16.c"
         "fonts/app_ui_font_cjk.c"
         "fonts/app_ui_font_cjk16.c"
         "fonts/app_ui_font_xl.c"
         "utf8_clamp.c"
         ${APP_SCREENSHOT_SRC}
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
    INCLUDE_DIRS "."
    # 串口截屏默认关闭（整屏缓冲 150KB 放不下，见 main/Kconfig.projbuild）

    REQUIRES bsp bt esp_driver_usb_serial_jtag esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
)

# ── 固件版本 ────────────────────────────────────────────────────────────────
# 基底版本：改 version.txt 一行即可（不要写死在代码里，否则必然忘记同步）。
# 覆盖方式：cmake -DAP_VERSION=1.0.0-rc1（整串替换，不再追加 git 描述）。
# 完整串 = <基底>+g<git 短哈希>[-dirty]；取不到 git 时只留基底。
if(NOT AP_VERSION)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../version.txt)
        file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../version.txt AP_VERSION_RAW)
        string(STRIP "${AP_VERSION_RAW}" AP_VERSION)
    endif()
    if(NOT AP_VERSION)
        set(AP_VERSION "0.0.0")
    endif()
    execute_process(
        COMMAND git describe --always --dirty --tags
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
        OUTPUT_VARIABLE AP_GIT_DESC
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(AP_GIT_DESC)
        set(AP_VERSION "${AP_VERSION}+g${AP_GIT_DESC}")
    endif()
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE AP_FIRMWARE_VERSION="${AP_VERSION}")
message(STATUS "固件版本：${AP_VERSION}（基底来自 version.txt，可用 -DAP_VERSION=… 覆盖）")
