#!/usr/bin/env bash
# ============================================================================
# Arbor Core — Deployment Script
#
# Deploys or updates the Arbor Control Server on a Raspberry Pi 5
# running Mainsail OS. Idempotent — safe to re-run.
#
# Usage:
#   ./deploy/deploy.sh              # Full first-time deploy
#   ./deploy/deploy.sh --update     # Update existing installation
#
# Prerequisites:
#   - Raspberry Pi 5 with Mainsail OS
#   - Run as the 'synthesis' user (or user with sudo)
#   - Git repo cloned to /opt/arbor
#
# Task: D01
# ============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration — override via environment if needed
# ---------------------------------------------------------------------------
ARBOR_USER="${ARBOR_USER:-synthesis}"
ARBOR_DIR="${ARBOR_DIR:-/opt/arbor}"
ARBOR_CONFIG_DIR="${ARBOR_CONFIG_DIR:-/etc/arbor}"
ARBOR_LOG_DIR="${ARBOR_LOG_DIR:-/var/log/arbor}"
ARBOR_DATA_DIR="${ARBOR_DATA_DIR:-/var/lib/arbor}"
ARBOR_VENV="${ARBOR_DIR}/.venv"
ARBOR_SERVICE="arbor-core"

# Derived paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
NGINX_CONF="${REPO_DIR}/deploy/nginx/arbor.conf"
SYSTEMD_UNIT="${REPO_DIR}/deploy/systemd/arbor.service"

# ---------------------------------------------------------------------------
# Helpers — respect NO_COLOR (https://no-color.org/) and non-TTY output
# ---------------------------------------------------------------------------
if [[ -n "${NO_COLOR:-}" ]] || [[ ! -t 1 ]]; then
    info()  { echo "[INFO]  $*"; }
    ok()    { echo "[OK]    $*"; }
    warn()  { echo "[WARN]  $*"; }
    error() { echo "[ERROR] $*" >&2; }
else
    info()  { echo -e "\033[1;34m[INFO]\033[0m  $*"; }
    ok()    { echo -e "\033[1;32m[OK]\033[0m    $*"; }
    warn()  { echo -e "\033[1;33m[WARN]\033[0m  $*"; }
    error() { echo -e "\033[1;31m[ERROR]\033[0m $*" >&2; }
fi
die()   { error "$@"; exit 1; }

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "Required command not found: $1"
}

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------
preflight() {
    info "Running pre-flight checks..."

    need_cmd python3
    need_cmd pip3
    need_cmd nginx
    need_cmd systemctl

    # Verify we're on a real Linux system (not Windows/WSL for dev)
    if [[ "$(uname -s)" != "Linux" ]]; then
        die "This script must be run on the Raspberry Pi (Linux), not $(uname -s)."
    fi

    # Check we can sudo
    if ! sudo -n true 2>/dev/null; then
        warn "sudo may require a password prompt."
    fi

    ok "Pre-flight checks passed."
}

# ---------------------------------------------------------------------------
# Create directory structure
# ---------------------------------------------------------------------------
create_directories() {
    info "Creating directory structure..."

    local dirs=("${ARBOR_CONFIG_DIR}" "${ARBOR_LOG_DIR}" "${ARBOR_DATA_DIR}")
    for dir in "${dirs[@]}"; do
        if [[ ! -d "$dir" ]]; then
            sudo mkdir -p "$dir"
            sudo chown "${ARBOR_USER}:${ARBOR_USER}" "$dir"
            ok "Created $dir"
        else
            ok "Exists: $dir"
        fi
    done
}

# ---------------------------------------------------------------------------
# Python virtual environment + dependencies
# ---------------------------------------------------------------------------
setup_venv() {
    info "Setting up Python virtual environment..."

    if [[ ! -d "${ARBOR_VENV}" ]]; then
        python3 -m venv "${ARBOR_VENV}"
        ok "Created venv at ${ARBOR_VENV}"
    else
        ok "Venv exists at ${ARBOR_VENV}"
    fi

    info "Installing/upgrading dependencies..."
    "${ARBOR_VENV}/bin/pip" install --upgrade pip --quiet
    "${ARBOR_VENV}/bin/pip" install -e "${ARBOR_DIR}/control-server/" --quiet
    ok "Python dependencies installed."
}

# ---------------------------------------------------------------------------
# Build WebUI (if source exists and node is available)
# ---------------------------------------------------------------------------
build_webui() {
    local webui_dir="${ARBOR_DIR}/webui"

    if [[ ! -d "$webui_dir" ]]; then
        warn "WebUI source directory not found at ${webui_dir}, skipping build."
        return 0
    fi

    if ! command -v npm >/dev/null 2>&1; then
        warn "npm not found — skipping WebUI build. Install Node.js to build the WebUI."
        return 0
    fi

    info "Building WebUI..."
    (cd "$webui_dir" && npm ci --quiet && npm run build)

    # Vite config writes output directly to control-server/arbor_core/webui/.
    # Verify the build produced an index.html in the target directory.
    local target="${ARBOR_DIR}/control-server/arbor_core/webui"
    if [[ -f "${target}/index.html" ]]; then
        ok "WebUI built successfully at ${target}"
    else
        warn "WebUI build completed but index.html not found at ${target}. Check vite.config.ts outDir."
    fi
}

# ---------------------------------------------------------------------------
# Default configuration
# ---------------------------------------------------------------------------
install_config() {
    local config_file="${ARBOR_CONFIG_DIR}/arbor.yaml"

    if [[ -f "$config_file" ]]; then
        ok "Configuration exists at ${config_file} — skipping (won't overwrite)."
        return 0
    fi

    info "Installing default configuration..."
    cat > "$config_file" << 'YAML'
# Arbor Core configuration — generated by deploy.sh
# See docs/DEPLOYMENT.md for full reference.

server:
  host: "0.0.0.0"
  port: 8000
  cors:
    # Allowed origins for CORS. Add your Pi's IP if .local doesn't resolve:
    #   - "http://192.168.x.x"
    # Empty list = allow all origins (development/provisioning only).
    allowed_origins:
      - "http://arbor.local"
      - "https://arbor.local"

security:
  api_key_min_length: 32
  jwt_algorithm: "RS256"
  jwt_expiry_minutes: 60
  rate_limit_default_rpm: 300

logging:
  level: "INFO"
  format: "json"
  output: "/var/log/arbor/arbor.log"

database:
  backend: "sqlite"
  path: "/var/lib/arbor/arbor.db"

# Node definitions — Phase 1 uses WiFi transport (HTTP to ESP32).
# The ESP32's IP is set during WiFi provisioning.
# Uncomment and set the IP after provisioning the board.
# nodes:
#   - id: "servo-board-01"
#     type: "esp32"
#     transport: "wifi"
#     transport_config:
#       host: "192.168.100.181"
#       network_port: 80

robotics:
  enabled: true
  moonraker:
    host: "localhost"
    port: 7125
    timeout: 5.0
    enabled: true

# Browsable file roots for the config editor (expert mode).
# source: "moonraker" proxies file ops through Moonraker API.
# source: "filesystem" reads/writes files directly on disk.
# restart_command: shell command or G-code to run after "Save & Restart".
file_roots:
  - id: klipper_config
    label: "Klipper Config"
    base_path: "/home/synthesis/printer_data/config"
    source: moonraker
    restart_command: "FIRMWARE_RESTART"
  - id: arbor_config
    label: "Arbor Config"
    base_path: "/etc/arbor"
    source: filesystem
    restart_command: "systemctl restart arbor-core"
YAML

    ok "Default configuration written to ${config_file}"
}

# ---------------------------------------------------------------------------
# nginx
# ---------------------------------------------------------------------------
install_nginx() {
    info "Configuring nginx..."

    if [[ ! -f "$NGINX_CONF" ]]; then
        die "nginx config not found: ${NGINX_CONF}"
    fi

    # Validate the new config BEFORE removing the old one
    # (use a temp copy so nginx -t can parse it)
    sudo cp "$NGINX_CONF" /etc/nginx/sites-available/arbor

    if ! sudo nginx -t 2>&1; then
        sudo rm -f /etc/nginx/sites-available/arbor
        die "nginx config validation failed! The new config has errors. Old config preserved."
    fi

    # Validation passed — now safe to swap configs
    if [[ -f /etc/nginx/sites-enabled/mainsail ]]; then
        sudo cp /etc/nginx/sites-enabled/mainsail /etc/nginx/mainsail.bak
        sudo rm -f /etc/nginx/sites-enabled/mainsail
        ok "Backed up Mainsail nginx config to /etc/nginx/mainsail.bak"
    fi

    sudo ln -sf /etc/nginx/sites-available/arbor /etc/nginx/sites-enabled/arbor
    sudo rm -f /etc/nginx/sites-enabled/default

    sudo systemctl reload nginx
    ok "nginx configured and reloaded."
}

# ---------------------------------------------------------------------------
# Check if nginx config needs updating (for --update mode)
# ---------------------------------------------------------------------------
check_nginx_config() {
    if [[ ! -f /etc/nginx/sites-available/arbor ]]; then
        warn "nginx config not installed. Run without --update for first-time setup."
        return 0
    fi

    if ! diff -q "$NGINX_CONF" /etc/nginx/sites-available/arbor >/dev/null 2>&1; then
        warn "nginx config has changed in the repo. Run install_nginx or full deploy to update."
        warn "  Repo:      ${NGINX_CONF}"
        warn "  Installed: /etc/nginx/sites-available/arbor"
    fi
}

# ---------------------------------------------------------------------------
# systemd service
# ---------------------------------------------------------------------------
install_service() {
    info "Installing systemd service..."

    if [[ ! -f "$SYSTEMD_UNIT" ]]; then
        die "Service unit not found: ${SYSTEMD_UNIT}"
    fi

    sudo cp "$SYSTEMD_UNIT" "/etc/systemd/system/${ARBOR_SERVICE}.service"
    sudo systemctl daemon-reload
    sudo systemctl enable "${ARBOR_SERVICE}"
    ok "Systemd service installed and enabled."
}

# ---------------------------------------------------------------------------
# Start / restart service
# ---------------------------------------------------------------------------
start_service() {
    info "Starting ${ARBOR_SERVICE}..."
    sudo systemctl restart "${ARBOR_SERVICE}"

    # Wait a moment and check status
    sleep 2
    if systemctl is-active --quiet "${ARBOR_SERVICE}"; then
        ok "${ARBOR_SERVICE} is running."
    else
        warn "${ARBOR_SERVICE} may not have started. Check: journalctl -u ${ARBOR_SERVICE} -n 20"
    fi
}

# ---------------------------------------------------------------------------
# Verify deployment
# ---------------------------------------------------------------------------
verify() {
    info "Verifying deployment..."

    local failures=0

    # Check services
    for svc in "${ARBOR_SERVICE}" nginx; do
        if systemctl is-active --quiet "$svc"; then
            ok "Service ${svc} is active."
        else
            warn "Service ${svc} is NOT active."
            failures=$((failures + 1))
        fi
    done

    # Check port 8000 (Arbor Core)
    if ss -tlnp 2>/dev/null | grep -q ':8000 '; then
        ok "Port 8000 is listening."
    else
        warn "Port 8000 is NOT listening."
        failures=$((failures + 1))
    fi

    # Check health endpoint
    if command -v curl >/dev/null 2>&1; then
        local health
        health=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1/health 2>/dev/null || echo "000")
        if [[ "$health" == "200" ]]; then
            ok "Health endpoint returned 200."
        else
            warn "Health endpoint returned ${health}."
            failures=$((failures + 1))
        fi
    fi

    # Check WebUI index.html exists
    local webui_index="${ARBOR_DIR}/control-server/arbor_core/webui/index.html"
    if [[ -f "$webui_index" ]]; then
        ok "WebUI index.html found."
    else
        warn "WebUI index.html not found at ${webui_index}. WebUI will not load."
        failures=$((failures + 1))
    fi

    if [[ "$failures" -eq 0 ]]; then
        echo ""
        ok "Deployment verified successfully!"
    else
        echo ""
        warn "${failures} verification check(s) failed. Review warnings above."
    fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    echo ""
    echo "======================================"
    echo "  Arbor Core Deployment"
    echo "======================================"
    echo ""

    local mode="full"
    if [[ "${1:-}" == "--update" ]]; then
        mode="update"
    fi

    preflight

    if [[ "$mode" == "full" ]]; then
        create_directories
        setup_venv
        build_webui
        install_config
        install_nginx
        install_service
        start_service
        verify
    else
        info "Running update (pull + reinstall + restart)..."
        (cd "${ARBOR_DIR}" && git pull --ff-only)
        setup_venv
        build_webui
        check_nginx_config
        start_service
        verify
    fi

    echo ""
    info "Logs:     journalctl -u ${ARBOR_SERVICE} -f"
    info "Config:   ${ARBOR_CONFIG_DIR}/arbor.yaml"
    info "WebUI:    http://$(hostname).local/"
    echo ""
}

main "$@"
