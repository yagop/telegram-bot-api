#!/bin/sh
set -eu

TEMP_DIR=/tmp/telegram-bot-api

# Remove in-flight upload directories left behind by a previous run if the
# storage was reused: the server never reuses them, and nothing else cleans
# them up. The name pattern is TEMP_DIRECTORY_PREFIX in td::HttpReader plus
# six random characters.
mkdir -p "${TEMP_DIR}"
rm -rf "${TEMP_DIR}"/tdlib-server-tmp??????

exec /usr/local/bin/telegram-bot-api "$@"
