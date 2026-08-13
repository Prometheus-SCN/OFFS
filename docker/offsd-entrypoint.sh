#!/bin/sh
# Entry point for the OFFS daemon (offsd) container.
#
# All persistent state — block cache, peer store, and node identity — lives
# under /data. Mount /data as a volume so the node's identity, peer list, and
# block cache survive container restarts.
#
#   /data/cache   — block cache (100+ GB)
#   /data/data    — peer_store.cbor (node ID, friends, hebbian weights)
#   /data/certs   — node.pem + node-key.pem (QUIC TLS identity)
#
# On first start, a self-signed cert is generated at /data/certs/node.pem.
# On subsequent starts, the existing cert is reused so the node ID is stable.
#
# To use your own CA-signed cert, mount it read-only at /data/certs/node.pem
# and /data/certs/node-key.pem before the first start.

set -e

export OPENSSL_CONF=/etc/ssl/openssl.cnf

CERT_DIR=/data/certs
CERT_PATH="${CERT_DIR}/node.pem"
KEY_PATH="${CERT_DIR}/node-key.pem"

mkdir -p "${CERT_DIR}" /data/cache /data/data

if [ ! -f "${CERT_PATH}" ] || [ ! -f "${KEY_PATH}" ]; then
  echo "offsd-entrypoint: no cert at ${CERT_PATH}, generating self-signed cert"
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -keyout "${KEY_PATH}" -out "${CERT_PATH}" \
    -subj "/CN=offs-offsd" >/dev/null 2>&1
fi

# Build the offsd command. RELAY_URL and MAX_CAPACITY_BYTES can be set via
# environment variables (avoids --command-line quoting issues with az CLI).
EXTRA_ARGS=""
if [ -n "${RELAY_URL}" ]; then
  EXTRA_ARGS="${EXTRA_ARGS} --relay-url ${RELAY_URL}"
fi
if [ -n "${MAX_CAPACITY_BYTES}" ]; then
  EXTRA_ARGS="${EXTRA_ARGS} --max-capacity-bytes ${MAX_CAPACITY_BYTES}"
fi

exec offsd --foreground \
  --node-cert "${CERT_PATH}" \
  --node-key "${KEY_PATH}" \
  --cache-dir /data/cache \
  --data-dir /data/data \
  ${EXTRA_ARGS} \
  "$@"