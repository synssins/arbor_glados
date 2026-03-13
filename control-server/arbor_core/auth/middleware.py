"""
Authentication middleware — scope enforcement, rate limiting, audit logging.

Extracts Bearer tokens from the Authorization header, identifies whether
they are API keys or JWTs, verifies them, enforces scopes, applies rate
limits, and writes audit log entries for write/admin operations.

Per SECURITY.md:
- Unauthorized resource access returns 404 (not 403) to prevent enumeration.
- Emergency stop endpoint is exempt from rate limiting.
- All write operations produce an audit log entry.

Task: C07
"""

from __future__ import annotations

import time
from collections import defaultdict
from typing import Any

import structlog
from fastapi import Request, Response
from fastapi.responses import JSONResponse
from starlette.middleware.base import BaseHTTPMiddleware, RequestResponseEndpoint

from arbor_core.auth.api_keys import APIKeyManager, APIKeyRecord, APIKeyScope
from arbor_core.auth.jwt import JWTError, JWTManager

logger = structlog.get_logger(__name__)

# Paths that do NOT require authentication
_PUBLIC_PATHS: frozenset[str] = frozenset(
    {
        "/api/v1/health",
        "/api/docs",
        "/api/redoc",
        "/api/openapi.json",
    }
)

# Paths exempt from rate limiting (emergency stop must always work)
_RATE_LIMIT_EXEMPT_PATHS: frozenset[str] = frozenset(
    {
        "/api/v1/emergency-stop",
    }
)

# HTTP methods considered "write" operations (trigger audit log)
_WRITE_METHODS: frozenset[str] = frozenset({"POST", "PUT", "DELETE", "PATCH"})

# Mapping of path prefixes -> required minimum scope
_SCOPE_RULES: list[tuple[str, str, APIKeyScope]] = [
    # (path_prefix, method_or_*, required_scope)
    ("/api/v1/auth/api-keys", "POST", APIKeyScope.ADMIN),
    ("/api/v1/auth/api-keys", "DELETE", APIKeyScope.ADMIN),
    ("/api/v1/system/config", "PUT", APIKeyScope.ADMIN),
    ("/api/v1/system/restart", "POST", APIKeyScope.ADMIN),
    ("/api/v1/system/ota", "*", APIKeyScope.ADMIN),
    ("/api/v1/servo", "PUT", APIKeyScope.WRITE),
    ("/api/v1/servo", "POST", APIKeyScope.WRITE),
    ("/api/v1/stepper", "POST", APIKeyScope.WRITE),
    ("/api/v1/led", "PUT", APIKeyScope.WRITE),
    ("/api/v1/led", "POST", APIKeyScope.WRITE),
    ("/api/v1/audio", "POST", APIKeyScope.WRITE),
    ("/api/v1/audio", "PUT", APIKeyScope.WRITE),
    ("/api/v1/emergency-stop", "POST", APIKeyScope.WRITE),
    ("/api/v1/ws", "*", APIKeyScope.STREAM),
    ("/api/v1/events", "*", APIKeyScope.STREAM),
]


class AuthState:
    """
    Authentication state attached to each request.

    Accessible via ``request.state.auth``.
    """

    def __init__(
        self,
        authenticated: bool = False,
        auth_type: str = "none",
        subject: str = "",
        scopes: list[str] | None = None,
        key_id: str | None = None,
        api_key_record: APIKeyRecord | None = None,
    ) -> None:
        self.authenticated = authenticated
        self.auth_type = auth_type  # "api_key" | "jwt" | "none"
        self.subject = subject
        self.scopes = scopes or []
        self.key_id = key_id
        self.api_key_record = api_key_record


# ---------------------------------------------------------------------------
# Rate limiter (in-memory, per-key token bucket)
# ---------------------------------------------------------------------------
class _RateLimiter:
    """Simple sliding-window rate limiter keyed by identifier."""

    def __init__(self, default_rpm: int) -> None:
        self._default_rpm = default_rpm
        self._windows: dict[str, list[float]] = defaultdict(list)

    def check(self, key: str, limit_rpm: int | None = None) -> tuple[bool, int]:
        """
        Check if a request is allowed.

        Args:
            key: Identifier (API key ID or JWT subject).
            limit_rpm: Per-key override. Falls back to default_rpm.

        Returns:
            (allowed, retry_after_seconds). retry_after is 0 if allowed.
        """
        rpm = limit_rpm or self._default_rpm
        now = time.monotonic()
        window_start = now - 60.0

        # Prune old entries
        entries = self._windows[key]
        self._windows[key] = [t for t in entries if t > window_start]

        if len(self._windows[key]) >= rpm:
            oldest = self._windows[key][0]
            retry_after = int(oldest - window_start) + 1
            return False, max(retry_after, 1)

        self._windows[key].append(now)
        return True, 0


# ---------------------------------------------------------------------------
# Middleware
# ---------------------------------------------------------------------------
class AuthMiddleware(BaseHTTPMiddleware):
    """
    FastAPI middleware for authentication, authorization, rate limiting, and audit logging.

    Constructor args come from config — nothing is hardcoded.
    """

    def __init__(
        self,
        app: Any,
        api_key_manager: APIKeyManager,
        jwt_manager: JWTManager | None = None,
        default_rate_limit_rpm: int = 300,
    ) -> None:
        """
        Args:
            app: The ASGI application.
            api_key_manager: Manages API key verification.
            jwt_manager: Manages JWT verification. None disables JWT auth.
            default_rate_limit_rpm: Default requests per minute per identity.
        """
        super().__init__(app)
        self._api_key_mgr = api_key_manager
        self._jwt_mgr = jwt_manager
        self._rate_limiter = _RateLimiter(default_rate_limit_rpm)

    async def dispatch(
        self, request: Request, call_next: RequestResponseEndpoint
    ) -> Response:
        """Process each request through the auth pipeline."""
        path = request.url.path
        method = request.method

        # Public paths bypass auth entirely
        if self._is_public(path):
            request.state.auth = AuthState()
            return await call_next(request)

        # Extract and verify credentials
        auth_state = self._authenticate(request)
        request.state.auth = auth_state

        if not auth_state.authenticated:
            # Return 404 per SECURITY.md (prevent enumeration)
            return JSONResponse(
                status_code=404,
                content={"detail": "Not found"},
            )

        # Scope enforcement
        required_scope = self._get_required_scope(path, method)
        if required_scope and not self._has_scope(auth_state, required_scope):
            return JSONResponse(
                status_code=404,
                content={"detail": "Not found"},
            )

        # Rate limiting (exempt paths skip this)
        if not self._is_rate_limit_exempt(path):
            identity = auth_state.key_id or auth_state.subject
            per_key_rpm = (
                auth_state.api_key_record.rate_limit_rpm
                if auth_state.api_key_record
                else None
            )
            allowed, retry_after = self._rate_limiter.check(identity, per_key_rpm)
            if not allowed:
                return JSONResponse(
                    status_code=429,
                    content={"detail": "Too many requests"},
                    headers={"Retry-After": str(retry_after)},
                )

        # Call the actual endpoint
        response = await call_next(request)

        # Audit log for write operations
        if method in _WRITE_METHODS:
            self._audit_log(request, auth_state, response.status_code)

        return response

    # -- Internal helpers ----------------------------------------------------

    def _is_public(self, path: str) -> bool:
        """Check if a path is in the public (no-auth) set."""
        return path in _PUBLIC_PATHS

    def _is_rate_limit_exempt(self, path: str) -> bool:
        """Check if a path is exempt from rate limiting."""
        return path in _RATE_LIMIT_EXEMPT_PATHS

    def _authenticate(self, request: Request) -> AuthState:
        """Extract Bearer token and verify it."""
        auth_header = request.headers.get("authorization", "")
        if not auth_header.lower().startswith("bearer "):
            return AuthState()

        token = auth_header[7:].strip()
        if not token:
            return AuthState()

        # Try API key first
        record = self._api_key_mgr.verify_key(token)
        if record is not None:
            return AuthState(
                authenticated=True,
                auth_type="api_key",
                subject=record.name,
                scopes=[s.value for s in record.scopes],
                key_id=record.id,
                api_key_record=record,
            )

        # Try JWT
        if self._jwt_mgr is not None:
            try:
                payload = self._jwt_mgr.verify(token)
                return AuthState(
                    authenticated=True,
                    auth_type="jwt",
                    subject=payload.get("sub", ""),
                    scopes=payload.get("scopes", []),
                )
            except JWTError:
                pass

        return AuthState()

    def _get_required_scope(self, path: str, method: str) -> APIKeyScope | None:
        """Determine the required scope for a path + method combination."""
        for prefix, rule_method, scope in _SCOPE_RULES:
            if path.startswith(prefix) and (rule_method == "*" or rule_method == method):
                return scope

        # Default: read scope for GET, write scope for mutations
        if method in _WRITE_METHODS:
            return APIKeyScope.WRITE
        return APIKeyScope.READ

    def _has_scope(self, auth_state: AuthState, required: APIKeyScope) -> bool:
        """Check if the auth state has the required scope."""
        if "admin" in auth_state.scopes:
            return True
        return required.value in auth_state.scopes

    def _audit_log(
        self, request: Request, auth_state: AuthState, status_code: int
    ) -> None:
        """Write an audit log entry for write operations."""
        client_ip = request.client.host if request.client else "unknown"
        logger.info(
            "audit_log",
            method=request.method,
            path=request.url.path,
            status_code=status_code,
            auth_type=auth_state.auth_type,
            subject=auth_state.subject,
            key_id=auth_state.key_id,
            client_ip=client_ip,
        )
