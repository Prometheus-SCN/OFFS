#!/bin/sh
# Entry point for the OFFS relay container.
#
# Generates a self-signed TLS cert at first start (if one is not mounted at
# /etc/offs/certs/relay.pem and /etc/offs/certs/relay-key.pem) and then execs
# offs_relay with that cert. QUIC requires TLS even in allow_secure=false mode;
# the cert is used for encryption only and is never validated by peers unless
# --allow-secure is also passed.
#
# To use your own cert, mount /etc/offs/certs/relay.pem and
# /etc/offs/certs/relay-key.pem (read-only) and pass --cert/--key explicitly via
# the container command line — those flags take precedence over this script.
set -e

# The runtime image copies OpenSSL 3 libs from the builder (which look for the
# config at /usr/local/ssl/openssl.cnf), but installs the openssl CLI from apt
# (which ships /etc/ssl/openssl.cnf). Point the CLI at the system config so
# `openssl req` works without the builder's config file.
export OPENSSL_CONF=/etc/ssl/openssl.cnf

CERT_DIR=/etc/offs/certs
CERT_PATH="${CERT_DIR}/relay.pem"
KEY_PATH="${CERT_DIR}/relay-key.pem"

if [ ! -f "${CERT_PATH}" ] || [ ! -f "${KEY_PATH}" ]; then
  echo "relay-entrypoint: no cert mounted at ${CERT_PATH}, generating self-signed cert"
  mkdir -p "${CERT_DIR}"
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -keyout "${KEY_PATH}" -out "${CERT_PATH}" \
    -subj "/CN=offs-relay" >/dev/null 2>&1
fi

exec offs_relay --cert "${CERT_PATH}" --key "${KEY_PATH}" "$@"