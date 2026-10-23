#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

ini_export() {
  local ini_file="$1"
  while IFS= read -r line || [ -n "$line" ]; do
    # 去掉行尾 CR（Windows 换行）
    line="${line%$'\r'}"
    # 去掉前后空白
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"

    # 跳过空行与注释
    [[ -z "$line" || "$line" == \#* || "$line" == \;* ]] && continue
    # 跳过 section
    [[ "$line" == \[*\] ]] && continue
    # 必须包含 =
    [[ "$line" != *"="* ]] && continue

    local key="${line%%=*}"
    local val="${line#*=}"

    # trim key
    key="${key%"${key##*[![:space:]]}"}"
    key="${key#"${key%%[![:space:]]*}"}"
    # trim value 左右空白，但保留中间空格
    val="${val#"${val%%[![:space:]]*}"}"
    val="${val%"${val##*[![:space:]]}"}"

    # 去掉可选的包裹引号
    if [[ "$val" == \"*\" && "$val" == *\" ]]; then
      val="${val:1:-1}"
    elif [[ "$val" == \'*\' && "$val" == *\' ]]; then
      val="${val:1:-1}"
    fi

    # 将不合法的变量名字符替换为下划线
    local var="${key//[^A-Za-z0-9_]/_}"

    # 导出为环境变量（保留值中的空格）
    export "$var=$val"
  done < "$ini_file"
}

ini_export "config.ini"
export LOCAL_PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
export TEMP_DIR="temp"
export LOCAL_LIB_DIR="${LOCAL_PROJECT_ROOT}/${LOCAL_LIB_DIR}"
export LOCAL_RES_DIR="${LOCAL_PROJECT_ROOT}/web_engine/src/main/resources/resfile"


CHROMIUM_RES_FILES=(
  resources.pak
  chrome_100_percent.pak
  chrome_200_percent.pak
  icudtl.dat
  snapshot_blob.bin
  v8_context_snapshot.bin
)

CHROMIUM_RES_DIRS=(
  locales/
  resources/
)

CHROMIUM_LIBS=(
  libadapter.so
  libchrome_main_web.so
)


CEF_RES_FILES=(
  "${CHROMIUM_RES_FILES[@]}"
)

CEF_RES_DIRS=(
  "${CHROMIUM_RES_DIRS[@]}"
)

CEF_LIBS=(
  libadapter.so
  libcef.so
  libcefclient.so
  libcefsimple.so
)

ELECTRON_BIN_FILES=(
  electron
)

ELECTRON_RES_FILES=(
  "${CHROMIUM_RES_FILES[@]}"
  electron
)

ELECTRON_RES_DIRS=(
  "${CHROMIUM_RES_DIRS[@]}"
)

ELECTRON_LIBS=(
  libadapter.so
  libelectron.so
  libffmpeg.so
)

case "${PROJECT_TYPE:-}" in
  electron)
    LIBS=("${ELECTRON_LIBS[@]}")
    PRODUCT_ROOTS=("${LOCAL_PROJECT_ROOT}/electron")
    BIN_FILES=("${ELECTRON_BIN_FILES[@]}")
    RES_FILES=("${ELECTRON_RES_FILES[@]}")
    RES_DIRS=("${ELECTRON_RES_DIRS[@]}")
    ;;
  chromium)
    LIBS=("${CHROMIUM_LIBS[@]}")
    PRODUCT_ROOTS=("${LOCAL_PROJECT_ROOT}/chromium")
    BIN_FILES=()
    RES_FILES=("${CHROMIUM_RES_FILES[@]}")
    RES_DIRS=("${CHROMIUM_RES_DIRS[@]}")
    ;;
  chromium-126-pc)
    LIBS=("${CHROMIUM_LIBS[@]}")
    PRODUCT_ROOTS=("${LOCAL_PROJECT_ROOT}/product/pc")
    BIN_FILES=()
    RES_FILES=("${CHROMIUM_RES_FILES[@]}")
    RES_DIRS=("${CHROMIUM_RES_DIRS[@]}")
    ;;
  cef)
    LIBS=("${CEF_LIBS[@]}")
    PRODUCT_ROOTS=("${LOCAL_PROJECT_ROOT}/entry")
    BIN_FILES=()
    RES_FILES=("${CEF_RES_FILES[@]}")
    RES_DIRS=("${CEF_RES_DIRS[@]}")
    ;;
  *)
    echo "Error: Unknown PROJECT_TYPE='${PROJECT_TYPE:-}' (expected 'electron' 'chromium-126-pc' or 'chromium')" >&2
    exit 1
    ;;
esac
