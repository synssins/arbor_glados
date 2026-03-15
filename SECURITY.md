# SECURITY.md — Arbor Security Checklist

> Every file created or modified must be reviewed against this checklist before marking the task complete.
> Security failures are not technical debt — they are shipped vulnerabilities.

---

## Pre-Commit Checklist

Run through this for every code change:

### Authentication & Authorization
- [ ] Every API endpoint requires authentication (no public endpoints except health)
- [ ] Auth scope is verified at the handler level, not just the router
- [ ] Unauthorized resource access returns 404, not 403 (prevents enumeration)
- [ ] JWT algorithm is explicitly specified server-side (`RS256`) — never derived from token
- [ ] API key is stored hashed (Argon2id) — original never written to disk or log
- [ ] No API keys or JWT secrets in source code or config files committed to git

### Input Validation
- [ ] All request bodies validated with Pydantic before processing
- [ ] Servo position/speed values validated against per-servo configured limits
- [ ] String inputs have length limits defined
- [ ] Numeric inputs have min/max bounds enforced
- [ ] Enum inputs validated against allowlist
- [ ] File paths derived from user input use `safe_join()` — never raw concatenation

### Network Security
- [ ] TLS required — no plain HTTP code paths in production
- [ ] TLS certificate validation enabled (no `verify=False`)
- [ ] Connections use TLS 1.2 minimum — TLS 1.0/1.1 disabled
- [ ] WebSocket connections authenticate on connect (not just on first message)
- [ ] Host header validation — reject requests where `Host` does not match expected values (mitigates DNS rebinding on LAN-connected boards)
- [ ] CORS origins explicitly configured in `server.cors.allowed_origins` for production (wildcard only acceptable during provisioning/development)
- [ ] nginx reverse proxy does NOT add its own CORS headers — Arbor handles CORS at the application layer to prevent duplicate headers

### ESP32-Specific
- [ ] NVS keys are short (max 15 chars) and documented
- [ ] No sensitive values logged at any log level
- [ ] OTA updates: firmware hash verified before applying
- [ ] HTTPS server: client connections dropped on TLS handshake failure
- [ ] No default credentials compiled into firmware — all provisioned

### Secrets Management
- [ ] No secrets, API keys, or passwords in source code
- [ ] No secrets in config files that will be committed (use `.env` or vault references)
- [ ] `.gitignore` includes: `*.env`, `*.pem`, `*.key`, `secrets/`, `credentials/`
- [ ] Example config files use placeholder values (e.g., `YOUR_API_KEY_HERE`)

### Rate Limiting
- [ ] All write endpoints have rate limiting configured
- [ ] Emergency stop endpoint is exempt from rate limiting
- [ ] Rate limit response includes `Retry-After` header
- [ ] Per-key rate limits are separate from global limits

### Audit Logging
- [ ] All write operations produce an audit log entry
- [ ] Audit entries include: timestamp, API key ID (not key), IP, endpoint, params summary
- [ ] Auth failures are logged (for intrusion detection)
- [ ] Audit log cannot be deleted via API (read-only to non-admin)

### WebUI Security
- [ ] React JSX used for all rendering (no `dangerouslySetInnerHTML` without sanitization)
- [ ] Content Security Policy header present and restrictive
- [ ] JWT stored in httpOnly cookie — never localStorage
- [ ] CSRF protection on all state-changing operations
- [ ] Confirmation dialog before destructive operations (home, OTA, restart)
- [ ] Emergency stop is always accessible — not blocked by loading states
- [ ] Bus-busy state communicated to WebUI — controls disabled during scan/long operations to prevent user confusion and queued commands

### Hardware Safety (Unique to Arbor)
- [ ] Position commands are bounds-checked against `min_position` / `max_position` from config
- [ ] Speed commands are bounds-checked against `max_speed` from config
- [ ] AI key commands are additionally constrained by `ai_constraints` config section
- [ ] Emergency stop command sends torque-disable to ALL servos, stop to ALL steppers
- [ ] **E-stop is never blocked by long-running operations** — bus scan, bulk read, or any mutex-holding operation must be interruptible or release the lock periodically so E-stop can acquire it within 100ms (Phase 1 completion criteria #5)
- [ ] Bus scan (`scan_bus`) checks an abort flag between individual servo pings — allows E-stop to cancel the scan and take immediate effect
- [ ] E-stop meets 100ms deadline during admin operations (factory_reset, restore, set_id) — currently these block httpd for up to 500ms. Future: run E-stop on a dedicated high-priority task decoupled from httpd
- [ ] Config changes to limits require admin scope — not write scope

---

## Security Headers Required on All HTTP Responses

```
Strict-Transport-Security: max-age=31536000; includeSubDomains
Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'self'; form-action 'self';
X-Content-Type-Options: nosniff
X-Frame-Options: DENY
Referrer-Policy: strict-origin-when-cross-origin
```

For API responses (non-HTML):
```
Content-Type: application/json
X-Content-Type-Options: nosniff
```

---

## Dependency Security

When adding a new dependency (after human approval):
- [ ] Check package for known vulnerabilities (`pip audit` / `npm audit`)
- [ ] Pin to specific version — no floating ranges in production deps
- [ ] Review what the dependency does — understand what you're importing
- [ ] Check license compatibility

---

## Threat Reference for Hardware Control APIs

These attack scenarios are specific to robotic control systems:

| Threat | Arbor Mitigation |
|--------|----------------------|
| Command replay attack | Include nonce or timestamp in signed commands |
| Position flooding | Per-key command rate limit, hardware velocity limits |
| Torque abuse | Max torque limits per servo in config |
| Stepper runaway | Soft endstop limits + physical endstop wiring |
| LED epilepsy (rapid flash) | Minimum transition time configurable |
| Audio abuse | Volume maximum configurable, rate limited |
| Fan destruction | Min/max duty cycle enforced |
| Thermal damage | Temp sensor alerts → automatic shutdown at threshold |
| DNS rebinding | Host header validation middleware — reject unexpected Host headers |
| E-stop denial during scan | Bus scan must be interruptible (abort flag); ESP-IDF httpd is single-threaded so mutex-holding operations block E-stop handler |

---

*This document is authoritative for all security decisions.*
*Security architecture changes require quorum + human approval.*
