#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local test_dir

    python3 tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_utf8_clamp.c main/utf8_clamp.c \
        -o "${test_dir}/test_utf8_clamp"
    "${test_dir}/test_utf8_clamp"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_question_logic.c main/app_question_logic.c \
        -o "${test_dir}/test_question_logic"
    "${test_dir}/test_question_logic"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_conn_logic.c main/app_conn_logic.c \
        -o "${test_dir}/test_conn_logic"
    "${test_dir}/test_conn_logic"
    # 配对纯逻辑（阶段 C）：6 位码派生、常量时间比较、失败限流
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_pair_logic.c main/app_pair_logic.c \
        -o "${test_dir}/test_pair_logic"
    "${test_dir}/test_pair_logic"
    # 系统设置纯逻辑（阶段 B）：菜单表、取值映射、档位循环、空闲关机判定
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_settings_logic.c main/app_settings_logic.c \
        -o "${test_dir}/test_settings_logic"
    "${test_dir}/test_settings_logic"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    # 界面中文字体覆盖检查（AI Passport × DSH 联机应用）。
    # 这是**真机上踩过的坑**：LVGL 内置的 CJK 子集不覆盖本应用文案，
    # 48 个中文字里缺 23 个，那些位置显示成方框。字体子集从源码字面量自动提取，
    # 但若改了文案却忘了重新生成，就又会缺字 —— 而这个错误只有在真机上才看得见。
    # 放进静态门禁，让它在提交前就失败。
    #
    # 脚本在**外层项目**的 tools/ 下（本仓库被当作 vendor 使用时才有），
    # 相对本仓库根是 ../../tools/。独立 clone 本仓库时它不存在，跳过即可 ——
    # 上游仓库不该因为我们加的应用而多出一条硬依赖。
    font_script="${repo_root}/../../tools/make-fonts.py"
    if [[ -f "${font_script}" ]]; then
        python3 "${font_script}" --check
    fi

    # AI Passport × DSH 联机应用的协议逻辑测试。
    # 它是纯 C（不依赖 ESP-IDF/NimBLE/LVGL），因此能和其它 host 测试一样直接编译运行。
    # 分片边界、乱序、重复片、ACK 重传这些逻辑错了也能编译通过、也能跑起来，
    # 只在真机上表现为"偶发"——放到这里跑是毫秒级反馈。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_app_proto.c main/app_proto.c \
        -o "${test_dir}/test_app_proto"
    "${test_dir}/test_app_proto"
    # 多会话列表/详情纯逻辑（协议 v2）：扁平字段解码、选中/未读收敛、UTF-8 安全截断。
    # 字段错位的表现是"设备把 A 的标题挂在 B 行""选中的详情回到别的会话"，
    # 与 BLE 故障的外观几乎一样，必须在 host 侧钉死。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_session_logic.c main/app_session_logic.c main/app_proto.c \
        -o "${test_dir}/test_session_logic"
    "${test_dir}/test_session_logic"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    "${test_dir}/test_bsp_es8311_sleep_check"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"
    # Apple ld（macOS）不认识 GNU ld 的 --gc-sections，等价参数是 -dead_strip。
    local dead_strip_flag="-Wl,--gc-sections"
    if [[ "$(uname -s)" == "Darwin" ]]; then
        dead_strip_flag="-Wl,-dead_strip"
    fi
    for demo in audio low_power ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" "${dead_strip_flag}" \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    # LVGL 任务归属契约：跑在 app_task / 链路回调里的入口不许碰 LVGL。
    # 真机上踩过：识别结果回调直接调 show_panel/refresh_*，与 LVGL 任务并发访问
    # 导致内部链表损坏、lv_inv_area 死循环、整机冻死（只告警不复位）。
    # 这个错误编译通过与常规测试都发现不了，只有真机会冻死 —— 放进静态门禁。
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_lvgl_task_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
