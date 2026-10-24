#!/usr/bin/env bash
# Refresh the shared CA bundle (curl's export of Mozilla's roots). Projects
# copy it into their romdisk when they build. PICO-8 Player's shipped notice
# (romdisk/certs/README.txt) records the date and SHA-256 printed here.
set -euo pipefail

DESTINATION="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/cacert.pem"

curl --fail --location --proto '=https' --tlsv1.2 \
    https://curl.se/ca/cacert.pem \
    --output "${DESTINATION}"
echo "Updated ${DESTINATION}"
grep -m1 'Certificate data from Mozilla' "${DESTINATION}" || true
shasum -a 256 "${DESTINATION}"
