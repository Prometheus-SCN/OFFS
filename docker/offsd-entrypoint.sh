#!/bin/sh
# Entry point for the OFFS daemon (offsd) container.
#
# Generates a self-signed node cert at first start if none is mounted at
# /etc/offs/certs/node.pem and /etc/offs/certs/node-key.pem, then execs offsd
# with that cert. QUIC requires TLS even in allow_secure=false mode; the cert
# is used for encryption only and is never validated by peers unless
# --allow-secure is also passed.
#
# To use your own cert, mount /etc/offs/certs/node.pem and
# /etc/offs/certs/node-key.pem (read-only).
#
# To persist the node identity across restarts, mount /data (both cache and
# peer_store.cbor live there). Without a mounted /data, the node gets a new
# random identity on every container restart.

set -e

# The runtime image copies OpenSSL 3 libs from the builder (which look for the
# config at /usr/local/ssl/openssl.cnf), but installs the openssl CLI from apt
# (which ships /etc/ssl/openssl.cnf). Point the CLI at the system config so
# `openssl req` works without the builder's config file.
export OPENSSL_CONF=/etc/ssl/openssl.cnf

CERT_DIR=/etc/offs/certs
CERT_PATH="${CERT_DIR}/node.pem"
KEY_PATH="${CERT_DIR}/node-key.pem"

if [ ! -f "${CERT_PATH}" ] || [ ! -f "${KEY_PATH}" ]; then
  echo "offsd-entrypoint: no cert mounted at ${CERT_PATH}, generating self-signed cert"
  mkdir -p "${CERT_DIR}"
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -keyout "${KEY_PATH}" -out "${CERT_PATH}" \
    -subj "/CN=offs-offsd" >/dev/null 2>&1
fi

exec offsd --foreground \
  --node-cert "${CERT_PATH}" \
  --node-key "${KEY_PATH}" \
  "$@"