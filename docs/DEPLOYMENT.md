# Arbor Core — Raspberry Pi Deployment Guide

This guide covers deploying Arbor Core to a Raspberry Pi 5 running Mainsail OS.

## Prerequisites

- Raspberry Pi 5 (4GB+ RAM recommended)
- Mainsail OS installed and running (Klipper + Moonraker + nginx)
- Network access to the Pi via SSH
- Git repository accessible from the Pi

## Architecture Overview

```
┌──────────────┐
│   Browser     │
│ (port 80)     │
└──────┬───────┘
       │
┌──────▼───────┐
│    nginx      │  Static files + reverse proxy
│  (port 80)    │
├───────────────┤
│  /            │ → Arbor UI (static files)
│  /api/        │ → Arbor Core (port 8000)
│  /health      │ → Arbor Core (port 8000)
│  /moonraker/  │ → Moonraker (port 7125)
└───────────────┘
       │
┌──────▼───────┐     ┌────────────────┐
│  Arbor Core   │     │   Moonraker     │
│  (port 8000)  │     │   (port 7125)   │
│  uvicorn      │     │                 │
└───────────────┘     └────────────────┘
```

## Directory Layout

| Path | Owner | Purpose |
|------|-------|---------|
| `/opt/arbor/` | synthesis | Application code (git repo) |
| `/etc/arbor/` | synthesis | Configuration files |
| `/var/log/arbor/` | synthesis | Application logs |
| `/var/lib/arbor/` | synthesis | Database and runtime data |

## Deployment Steps

### 1. Install Prerequisites

```bash
sudo apt-get update -qq
sudo apt-get install -y python3-pip python3-venv nginx openssl
```

### 2. Create Directories

```bash
sudo mkdir -p /opt/arbor /etc/arbor /var/log/arbor /var/lib/arbor
sudo chown synthesis:synthesis /opt/arbor /etc/arbor /var/log/arbor /var/lib/arbor
```

### 3. Clone Repository

```bash
cd /opt/arbor
git clone https://github.com/synssins/arbor_glados.git .
```

### 4. Create Python Virtual Environment

```bash
cd /opt/arbor
python3 -m venv .venv
.venv/bin/pip install --upgrade pip
.venv/bin/pip install -e control-server/
```

### 5. Create Configuration

Create `/etc/arbor/arbor.yaml`:

```yaml
server:
  host: "0.0.0.0"
  port: 8000
  cors:
    # Set to your server's actual hostname/IP to lock down CORS.
    # Empty list defaults to wildcard (all origins) for development.
    allowed_origins:
      - "http://arbor.local"
      - "https://arbor.local"

security:
  api_key_min_length: 32
  api_key_hash_algorithm: "argon2id"
  jwt_algorithm: "RS256"
  jwt_expiry_minutes: 60
  jwt_private_key_path: "/etc/arbor/jwt/private.pem"
  jwt_public_key_path: "/etc/arbor/jwt/public.pem"
  rate_limit_rpm: 120
  max_api_keys: 50

logging:
  level: "info"
  format: "json"
  file: "/var/log/arbor/arbor.log"

database:
  url: "sqlite:///var/lib/arbor/arbor.db"

nodes:
  - id: "esp32-servo-01"
    name: "Servo Controller"
    type: "esp32"
    transport: "serial"
    serial_port: "/dev/ttyUSB0"
    serial_baud: 115200
```

### 6. Configure nginx

Replace the Mainsail nginx config with the Arbor config. Back up the original first:

```bash
sudo cp /etc/nginx/sites-enabled/mainsail /etc/nginx/mainsail.bak
sudo rm /etc/nginx/sites-enabled/mainsail
```

Create `/etc/nginx/sites-enabled/arbor` with:
- Static file serving from `/opt/arbor/control-server/arbor_core/webui`
- Reverse proxy `/api/` → `http://127.0.0.1:8000`
- Reverse proxy `/health` → `http://127.0.0.1:8000`
- WebSocket proxy `/api/v1/ws` → `http://127.0.0.1:8000`
- Moonraker proxy `/moonraker/` → `http://127.0.0.1:7125`
- SPA routing (all non-API paths → `index.html`)
- gzip compression enabled

Test and reload:
```bash
sudo nginx -t
sudo systemctl reload nginx
```

### 7. Install systemd Service

Create `/etc/systemd/system/arbor-core.service`:

```ini
[Unit]
Description=Arbor Core Control Server
After=network-online.target moonraker.service
Wants=network-online.target

[Service]
Type=simple
User=synthesis
Group=synthesis
WorkingDirectory=/opt/arbor
Environment=ARBOR_CONFIG=/etc/arbor/arbor.yaml
ExecStart=/opt/arbor/.venv/bin/uvicorn arbor_core.main:app --host 127.0.0.1 --port 8000 --workers 1
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=arbor-core

# Hardening
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=read-only
ReadWritePaths=/var/lib/arbor /var/log/arbor /etc/arbor
PrivateTmp=true

# Hardware access
SupplementaryGroups=dialout gpio spi i2c

[Install]
WantedBy=multi-user.target
```

Enable and start:
```bash
sudo systemctl daemon-reload
sudo systemctl enable arbor-core
sudo systemctl start arbor-core
```

## Verification

```bash
# All services should be active
systemctl is-active arbor-core nginx klipper moonraker

# Ports should be listening
ss -tlnp | grep -E ':80 |:8000 |:7125 '

# Health checks
curl -s http://127.0.0.1/health
curl -s http://127.0.0.1/api/v1/system/health

# Moonraker still accessible
curl -s http://127.0.0.1/moonraker/server/info
```

## Updating

```bash
cd /opt/arbor
git pull origin main
.venv/bin/pip install -e control-server/
sudo systemctl restart arbor-core
```

## Logs

```bash
# Arbor Core logs
sudo journalctl -u arbor-core -f

# nginx access/error logs
tail -f /var/log/nginx/arbor-access.log
tail -f /var/log/nginx/arbor-error.log
```

## Troubleshooting

| Symptom | Check |
|---------|-------|
| Service won't start | `journalctl -u arbor-core -n 50` |
| nginx 502 Bad Gateway | Is arbor-core running? `systemctl status arbor-core` |
| Port 8000 not listening | Check uvicorn logs in journal |
| Moonraker inaccessible | Verify `/moonraker/` proxy in nginx config |
| Import errors | Reinstall: `.venv/bin/pip install -e control-server/` |

## Security Notes

- Arbor Core binds to `127.0.0.1:8000` (localhost only) — all external access goes through nginx
- systemd hardening: `NoNewPrivileges`, `ProtectSystem=strict`, `PrivateTmp`
- The `synthesis` user runs both Klipper ecosystem services and Arbor Core
- SSH credentials and deployment scripts are in `.claude/` (gitignored, never committed)
- **CORS**: Arbor handles CORS at the application layer. Do NOT add CORS headers (`add_header Access-Control-*`) in nginx — this causes duplicate headers that browsers reject. Configure origins in `server.cors.allowed_origins` in `arbor.yaml`.
- **CORS in production**: Always set explicit `allowed_origins` in the config. The wildcard fallback (empty list) is for first-boot/development only.
