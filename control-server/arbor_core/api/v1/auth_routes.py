"""
Auth API endpoints — login, logout, api-key CRUD.

Per ARBOR_PROJECT_PLAN.md Section 6:
    POST /api/v1/auth/login
    POST /api/v1/auth/logout
    POST /api/v1/auth/api-keys       # create API key (admin)
    GET  /api/v1/auth/api-keys       # list keys (admin)
    DELETE /api/v1/auth/api-keys/{id} # revoke key (admin)

Task: C15
"""

from __future__ import annotations

from typing import Any

import structlog
from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field

from arbor_core.auth.api_keys import APIKeyManager, APIKeyScope

logger = structlog.get_logger(__name__)

auth_router = APIRouter(prefix="/auth", tags=["auth"])


# ---------------------------------------------------------------------------
# Request / Response models
# ---------------------------------------------------------------------------
class LoginRequest(BaseModel):
    """Login request body."""

    username: str = Field(min_length=1, max_length=128)
    password: str = Field(min_length=1, max_length=256)


class CreateAPIKeyRequest(BaseModel):
    """Request body for creating a new API key."""

    name: str = Field(min_length=1, max_length=64, description="Human-readable key label.")
    scopes: list[APIKeyScope] = Field(
        min_length=1, description="Scopes to grant."
    )
    rate_limit_rpm: int | None = Field(
        default=None, ge=1, description="Per-key rate limit override."
    )
    ip_allowlist: list[str] = Field(
        default_factory=list, description="Optional IP allowlist."
    )


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _get_api_key_manager(request: Request) -> APIKeyManager:
    """Extract APIKeyManager from app state."""
    mgr: APIKeyManager | None = getattr(request.app.state, "api_key_manager", None)
    if mgr is None:
        msg = "APIKeyManager not configured in app state"
        raise RuntimeError(msg)
    return mgr


def _get_jwt_manager(request: Request) -> Any:
    """Extract JWTManager from app state."""
    mgr = getattr(request.app.state, "jwt_manager", None)
    if mgr is None:
        msg = "JWTManager not configured in app state"
        raise RuntimeError(msg)
    return mgr


def _require_admin(request: Request) -> bool:
    """Check that the request has admin scope. Returns False if not."""
    auth = getattr(request.state, "auth", None)
    if auth is None or not auth.authenticated:
        return False
    return "admin" in auth.scopes


# ---------------------------------------------------------------------------
# Endpoints
# ---------------------------------------------------------------------------
@auth_router.post(
    "/login",
    summary="Login and receive JWT",
    description="Authenticate with username/password and receive a JWT token.",
)
async def login(body: LoginRequest, request: Request) -> JSONResponse:
    """
    Login endpoint. Issues a JWT token on successful authentication.

    For Phase 1, this validates against the admin credentials in config.
    Full user management is Phase 3.
    """
    jwt_mgr = _get_jwt_manager(request)

    # Phase 1: simple credential check against config
    # Full user management is out of scope for Phase 1
    admin_user = getattr(request.app.state, "admin_username", None)
    admin_pass = getattr(request.app.state, "admin_password_hash", None)

    if admin_user is None or admin_pass is None:
        return JSONResponse(
            status_code=404,
            content={"detail": "Not found"},
        )

    # Verify credentials (using argon2 if available)
    try:
        from argon2 import PasswordHasher
        from argon2.exceptions import VerifyMismatchError

        hasher = PasswordHasher()
        if body.username != admin_user:
            return JSONResponse(status_code=404, content={"detail": "Not found"})
        try:
            hasher.verify(admin_pass, body.password)
        except VerifyMismatchError:
            logger.warning("login_failed", username=body.username)
            return JSONResponse(status_code=404, content={"detail": "Not found"})
    except ImportError:
        return JSONResponse(
            status_code=500,
            content={"detail": "Auth backend not available"},
        )

    token = jwt_mgr.issue(
        subject=body.username,
        scopes=["read", "write", "admin", "stream"],
    )

    logger.info("login_success", username=body.username)
    return JSONResponse(
        status_code=200,
        content={"token": token, "token_type": "bearer"},
    )


@auth_router.post(
    "/logout",
    summary="Logout (invalidate session)",
    description="Logout the current session. JWT invalidation is client-side for Phase 1.",
)
async def logout(request: Request) -> JSONResponse:
    """
    Logout endpoint.

    For Phase 1 with stateless JWTs, logout is handled client-side
    by discarding the token. Server-side token blocklist is Phase 2.
    """
    auth = getattr(request.state, "auth", None)
    subject = auth.subject if auth else "unknown"
    logger.info("logout", subject=subject)
    return JSONResponse(
        status_code=200,
        content={"detail": "Logged out"},
    )


@auth_router.post(
    "/api-keys",
    summary="Create API key (admin only)",
    description="Create a new API key. The plaintext key is returned once.",
    status_code=201,
)
async def create_api_key(body: CreateAPIKeyRequest, request: Request) -> JSONResponse:
    """Create a new API key. Requires admin scope."""
    if not _require_admin(request):
        return JSONResponse(status_code=404, content={"detail": "Not found"})

    mgr = _get_api_key_manager(request)
    result = mgr.create_key(
        name=body.name,
        scopes=body.scopes,
        rate_limit_rpm=body.rate_limit_rpm,
        ip_allowlist=body.ip_allowlist,
    )

    logger.info("api_key_created_via_api", key_id=result.id, name=result.name)

    return JSONResponse(
        status_code=201,
        content={
            "id": result.id,
            "name": result.name,
            "plaintext_key": result.plaintext_key,
            "prefix": result.prefix,
            "scopes": [s.value for s in result.scopes],
            "created_at": result.created_at.isoformat(),
        },
    )


@auth_router.get(
    "/api-keys",
    summary="List API keys (admin only)",
    description="List all API keys with metadata (no hashes exposed).",
)
async def list_api_keys(request: Request) -> JSONResponse:
    """List all API keys. Requires admin scope."""
    if not _require_admin(request):
        return JSONResponse(status_code=404, content={"detail": "Not found"})

    mgr = _get_api_key_manager(request)
    keys = mgr.list_keys()

    return JSONResponse(status_code=200, content={"keys": keys})


@auth_router.delete(
    "/api-keys/{key_id}",
    summary="Revoke API key (admin only)",
    description="Revoke an API key by its ID.",
)
async def revoke_api_key(key_id: str, request: Request) -> JSONResponse:
    """Revoke an API key. Requires admin scope."""
    if not _require_admin(request):
        return JSONResponse(status_code=404, content={"detail": "Not found"})

    mgr = _get_api_key_manager(request)
    revoked = mgr.revoke_key(key_id)

    if not revoked:
        return JSONResponse(status_code=404, content={"detail": "Not found"})

    logger.info("api_key_revoked_via_api", key_id=key_id)
    return JSONResponse(status_code=200, content={"detail": "Key revoked"})
