#!/usr/bin/env bash
set -euo pipefail

version="1.7.12"
destination="${1:-/tmp/actionlint-${version}}"
system_name="$(uname -s)"
machine_name="$(uname -m)"

if [[ -x "${destination}/actionlint" ]]; then
    printf '%s\n' "${destination}/actionlint"
    exit 0
fi

case "${system_name}/${machine_name}" in
    Linux/x86_64)
        platform="linux_amd64"
        checksum="8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8"
        ;;
    Linux/aarch64|Linux/arm64)
        platform="linux_arm64"
        checksum="325e971b6ba9bfa504672e29be93c24981eeb1c07576d730e9f7c8805afff0c6"
        ;;
    Darwin/x86_64)
        platform="darwin_amd64"
        checksum="5b44c3bc2255115c9b69e30efc0fecdf498fdb63c5d58e17084fd5f16324c644"
        ;;
    Darwin/arm64)
        platform="darwin_arm64"
        checksum="aba9ced2dee8d27fecca3dc7feb1a7f9a52caefa1eb46f3271ea66b6e0e6953f"
        ;;
    *)
        echo "Unsupported actionlint platform: ${system_name}/${machine_name}" >&2
        exit 1
        ;;
esac

archive_name="actionlint_${version}_${platform}.tar.gz"
archive_path="${destination}/${archive_name}"
download_url="https://github.com/rhysd/actionlint/releases/download/v${version}/${archive_name}"

mkdir -p "${destination}"
if ! curl --fail --location --silent --show-error --retry 3 --retry-all-errors \
    "${download_url}" --output "${archive_path}"; then
    if ! command -v gh >/dev/null 2>&1; then
        echo "actionlint download failed and GitHub CLI is unavailable" >&2
        exit 1
    fi
    rm -f "${archive_path}"
    gh release download "v${version}" --repo rhysd/actionlint \
        --pattern "${archive_name}" --dir "${destination}"
fi
# 校验工具的选择必须**实测能力**，不能只看命令是否存在。
# macOS 自带 /sbin/sha256sum（BSD 版），它不认识 GNU 的 --check/--status：
# `command -v sha256sum` 会成功，但紧接着 `sha256sum --check` 打印 usage 并以非零退出，
# 于是整个门禁在"校验下载包"这一步失败，报错却只显示一行 usage，很难定位。
# 因此这里先跑一次真实调用，能用才用它，否则退回 shasum。
if command -v sha256sum >/dev/null 2>&1 \
    && printf '%s  %s\n' "${checksum}" "${archive_path}" | sha256sum --check --status >/dev/null 2>&1; then
    :
elif command -v shasum >/dev/null 2>&1; then
    [[ "$(shasum -a 256 "${archive_path}" | awk '{print $1}')" == "${checksum}" ]] || {
        echo "actionlint archive checksum mismatch" >&2
        exit 1
    }
else
    echo "No working SHA-256 verification tool is available" >&2
    exit 1
fi
tar -xzf "${archive_path}" -C "${destination}" actionlint
chmod +x "${destination}/actionlint"
printf '%s\n' "${destination}/actionlint"
