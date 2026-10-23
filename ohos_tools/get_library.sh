#!/usr/bin/env bash

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
source "${SCRIPT_DIR}/env.sh"

SSH_PORT="${SSH_PORT:-22}"
SCP_OPTS=(-P "$SSH_PORT" -o StrictHostKeyChecking=accept-new)
if [ -n "${SSH_KEY:-}" ]; then
  SCP_OPTS+=(-i "$SSH_KEY")
fi

mkdir -p "${TEMP_DIR}"
mkdir -p "${LOCAL_LIB_DIR}"

echo "Downloading files from ${REMOTE_HOST}:${PROJECT_ROOT}/${BUILD_DIR} into ${LOCAL_LIB_DIR}"

for bin in "${BIN_FILES[@]}"; do
  scp "${SCP_OPTS[@]}" "${REMOTE_USER}@${REMOTE_HOST}:${PROJECT_ROOT}/${BUILD_DIR}/${bin}" "${TEMP_DIR}"
done

if [ ! -f "${LOCAL_LIB_DIR}/libc++_shared.so" ]; then
  scp "${SCP_OPTS[@]}" "${REMOTE_USER}@${REMOTE_HOST}:${PROJECT_ROOT}/${LLVM_DIR}/lib/aarch64-linux-ohos/libc++_shared.so" "${TEMP_DIR}"
  cp -f "${TEMP_DIR}/libc++_shared.so" "${LOCAL_LIB_DIR}/"
fi

for lib in "${LIBS[@]}"; do
  scp "${SCP_OPTS[@]}" "${REMOTE_USER}@${REMOTE_HOST}:${PROJECT_ROOT}/${BUILD_DIR}/${lib}" "${TEMP_DIR}"
  cp -f "${TEMP_DIR}/${lib}" "${LOCAL_LIB_DIR}/"
done

echo "==============================================================================="
echo "=                         You can run the APP now.                            ="
echo "==============================================================================="
echo "current time: $(date '+%T')"

#echo "Begin to fetch unstripped file."
#mkdir -p "${TEMP_DIR}/unstripped"
#for lib in "${LIBS[@]}"; do
#  scp "${REMOTE_USER}@${REMOTE_HOST}:${PROJECT_ROOT}/${BUILD_DIR}/lib.unstripped/${lib}" "${TEMP_DIR}/unstripped/"
#done
