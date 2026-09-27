#!/bin/bash
set -e

# environment variables for crossdesk-server
CROSSDESK_SERVER_PORT=${CROSSDESK_SERVER_PORT:-9090}
# Optional admin dashboard environment variables:
#   ADMIN_USERNAME and ADMIN_PASSWORD
# If both are set, crossdesk-server enables /admin on the HTTPS port. The
# values are inherited by the server process below.

is_uint() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

# check environment variables
if [ -z "$EXTERNAL_IP" ]; then
  echo "Error: EXTERNAL_IP must be set."
  echo "Set EXTERNAL_IP in .env before starting the Compose stack."
  exit 1
fi

for port_name in CROSSDESK_SERVER_PORT; do
  port_value="${!port_name}"
  if ! is_uint "$port_value" || [ "$port_value" -lt 1 ] || [ "$port_value" -gt 65535 ]; then
    echo "Error: $port_name must be an integer between 1 and 65535."
    exit 1
  fi
done

# check and generate certificates if needed
CERT_DIR="/var/lib/crossdesk/certs"
DB_DIR="/var/lib/crossdesk/db"
LOG_DIR="/var/log/crossdesk"
CERT_KEY="$CERT_DIR/api.crossdesk.cn.key"
CERT_BUNDLE="$CERT_DIR/api.crossdesk.cn_bundle.crt"
CERT_ROOT="$CERT_DIR/api.crossdesk.cn_root.crt"

mkdir -p "$CERT_DIR" "$DB_DIR" "$LOG_DIR"
# Coturn drops to nobody:nogroup (65534:65534 in the pinned Debian image).
# Its daily files share the server's retention policy through this mount.
if [ -L "$LOG_DIR/coturn" ]; then
  echo "Error: Coturn log directory must not be a symbolic link."
  exit 1
fi
mkdir -p "$LOG_DIR/coturn"
chown 65534:65534 "$LOG_DIR/coturn"
chmod 750 "$LOG_DIR/coturn"

if [ ! -f "$CERT_KEY" ] || [ ! -f "$CERT_BUNDLE" ]; then
  echo "Certificate files not found, generating certificates..."
  
  # Run generate_certs.sh with EXTERNAL_IP and output directory
  bash /docker/generate_certs.sh "$EXTERNAL_IP" "$CERT_DIR"
  
  # Verify certificates were generated
  if [ ! -f "$CERT_KEY" ] || [ ! -f "$CERT_BUNDLE" ] || [ ! -f "$CERT_ROOT" ]; then
    echo "Error: Failed to generate certificate files"
    exit 1
  fi
  
  echo "Certificates generated successfully"
else
  echo "Certificate files found, skipping generation"
fi

# start crossdesk-server as main foreground process
echo "Starting crossdesk-server..."
echo "Certificate directory: $CERT_DIR"
echo "Certificate files:"
ls -la "$CERT_DIR" || echo "Warning: Cannot list certificate directory"

exec ./crossdesk-server/crossdesk_server ${CROSSDESK_SERVER_PORT}
