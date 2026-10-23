#!/usr/bin/env bash

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

source "${SCRIPT_DIR}/env.sh"

SSH_PORT="${SSH_PORT:-22}"
SCP_OPTS=(-P "$SSH_PORT" -o StrictHostKeyChecking=accept-new)
if [ -n "${SSH_KEY:-}" ]; then
  SCP_OPTS+=(-i "$SSH_KEY")
fi

mkdir -p "${TEMP_DIR}"

echo "login as user: ${REMOTE_USER}"
echo "Downloading resources from ${REMOTE_HOST}:${PROJECT_ROOT}/${BUILD_DIR}"

remote_base="${REMOTE_USER}@${REMOTE_HOST}:${PROJECT_ROOT}/${BUILD_DIR}"

for res_file in "${RES_FILES[@]}"; do
    scp "${SCP_OPTS[@]}" "${remote_base}/${res_file}" "${TEMP_DIR}/"
done

for res_dir in "${RES_DIRS[@]}"; do
    scp -r "${SCP_OPTS[@]}" "${remote_base}/${res_dir}" "${TEMP_DIR}/"
done

echo "copy files into 'web_engine/src/main/resources/resfile'"
mkdir -p "$LOCAL_RES_DIR"

cp -af "$TEMP_DIR"/*.pak                    "$LOCAL_RES_DIR"/
cp -af "$TEMP_DIR/icudtl.dat"               "$LOCAL_RES_DIR"/
cp -af "$TEMP_DIR/resources.pak"            "$LOCAL_RES_DIR"/
cp -af "$TEMP_DIR/snapshot_blob.bin"        "$LOCAL_RES_DIR"/
cp -af "$TEMP_DIR/v8_context_snapshot.bin"  "$LOCAL_RES_DIR"/

mkdir -p "$LOCAL_RES_DIR/locales" "$LOCAL_RES_DIR/resources"
cp -af "$TEMP_DIR/locales/."    "$LOCAL_RES_DIR/locales/"
cp -af "$TEMP_DIR/resources/."  "$LOCAL_RES_DIR/resources/"
