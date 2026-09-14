#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DESTINATION="${PROJECT_DIR}/romdisk/cacert.pem"

mkdir -p "${PROJECT_DIR}/romdisk"
curl --fail --location --proto '=https' --tlsv1.2 \
    https://curl.se/ca/cacert.pem \
    --output "${DESTINATION}"
echo "Updated ${DESTINATION}"
