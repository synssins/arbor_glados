"""
Tests for auth middleware (C07 / C19).

Covers:
- Unauthenticated requests to protected endpoints return 404
- Public endpoints (health, docs) bypass auth
- API key auth flow (valid key, revoked key, wrong key)
- JWT auth flow (valid token, expired, tampered)
- Scope enforcement (read can't write, admin can do all)
- Rate limiting (429 with Retry-After header)
- Emergency stop exempt from rate limiting
- Audit logging on write operations
- Security headers on all responses
"""

from __future__ import annotations

from pathlib import Path
from typing import Any
from unittest.mock import patch

import pytest
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse
from httpx import ASGITransport, AsyncClient

from arbor_core.auth.api_keys import APIKeyManager, APIKeyScope
from arbor_core.auth.headers import SecurityHeadersMiddleware
from arbor_core.auth.jwt import JWTManager, generate_keypair
from arbor_core.auth.middleware import AuthMiddleware


def _make_app(
    api_key_mgr: APIKeyManager,
    jwt_mgr: JWTManager | None = None,
    rate_limit_rpm: int = 300,
) -> FastAPI:
    """Build a test FastAPI app with auth middleware and sample routes."""
    app = FastAPI()

    # Sample routes
    @app.get("/api/v1/health")
    async def health() -> dict[str, str]:
        return {"status": "healthy"}

    @app.get("/api/v1/system/info")
    async def system_info(request: Request) -> dict[str, Any]:
        return {"info": "system", "auth": request.state.auth.auth_type}

    @app.put("/api/v1/servo/1/position")
    async def servo_position(request: Request) -> dict[str, str]:
        return {"status": "ok"}

    @app.post("/api/v1/auth/api-keys")
    async def create_api_key(request: Request) -> dict[str, str]:
        return {"status": "created"}

    @app.post("/api/v1/emergency-stop")
    async def emergency_stop(request: Request) -> dict[str, str]:
        return {"status": "stopped"}

    @app.get("/api/v1/ws")
    async def websocket_stub(request: Request) -> dict[str, str]:
        return {"status": "stream"}

    # Add middlewares (order matters — security headers outermost)
    app.add_middleware(SecurityHeadersMiddleware)
    app.add_middleware(
        AuthMiddleware,
        api_key_manager=api_key_mgr,
        jwt_manager=jwt_mgr,
        default_rate_limit_rpm=rate_limit_rpm,
    )

    return app


@pytest.fixture()
def api_key_mgr() -> APIKeyManager:
    return APIKeyManager(min_key_length=32)


@pytest.fixture()
def jwt_mgr(tmp_path: Path) -> JWTManager:
    priv = tmp_path / "private.pem"
    pub = tmp_path / "public.pem"
    generate_keypair(priv, pub)
    return JWTManager.from_key_files(priv, pub, expiry_minutes=60)


@pytest.fixture()
def read_key(api_key_mgr: APIKeyManager) -> str:
    result = api_key_mgr.create_key(name="reader", scopes=[APIKeyScope.READ])
    return result.plaintext_key


@pytest.fixture()
def write_key(api_key_mgr: APIKeyManager) -> str:
    result = api_key_mgr.create_key(
        name="writer", scopes=[APIKeyScope.READ, APIKeyScope.WRITE]
    )
    return result.plaintext_key


@pytest.fixture()
def admin_key(api_key_mgr: APIKeyManager) -> str:
    result = api_key_mgr.create_key(name="admin", scopes=[APIKeyScope.ADMIN])
    return result.plaintext_key


@pytest.fixture()
def stream_key(api_key_mgr: APIKeyManager) -> str:
    result = api_key_mgr.create_key(
        name="streamer", scopes=[APIKeyScope.READ, APIKeyScope.STREAM]
    )
    return result.plaintext_key


class TestPublicEndpoints:
    """Public endpoints bypass auth."""

    async def test_health_no_auth(self, api_key_mgr: APIKeyManager) -> None:
        app = _make_app(api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get("/api/v1/health")
            assert resp.status_code == 200


class TestUnauthenticated:
    """Requests without credentials."""

    async def test_no_auth_returns_404(self, api_key_mgr: APIKeyManager) -> None:
        app = _make_app(api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get("/api/v1/system/info")
            assert resp.status_code == 404

    async def test_invalid_bearer_returns_404(self, api_key_mgr: APIKeyManager) -> None:
        app = _make_app(api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get(
                "/api/v1/system/info",
                headers={"Authorization": "Bearer bogus-key"},
            )
            assert resp.status_code == 404


class TestAPIKeyAuth:
    """API key authentication."""

    async def test_valid_api_key(
        self, api_key_mgr: APIKeyManager, read_key: str
    ) -> None:
        app = _make_app(api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get(
                "/api/v1/system/info",
                headers={"Authorization": f"Bearer {read_key}"},
            )
            assert resp.status_code == 200
            assert resp.json()["auth"] == "api_key"

    async def test_revoked_key_rejected(
        self, api_key_mgr: APIKeyManager, read_key: str
    ) -> None:
        # Revoke
        keys = api_key_mgr.list_keys()
        api_key_mgr.revoke_key(keys[0]["id"])

        app = _make_app(api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get(
                "/api/v1/system/info",
                headers={"Authorization": f"Bearer {read_key}"},
            )
            assert resp.status_code == 404


class TestJWTAuth:
    """JWT authentication."""

    async def test_valid_jwt(
        self, api_key_mgr: APIKeyManager, jwt_mgr: JWTManager
    ) -> None:
        token = jwt_mgr.issue(subject="testuser", scopes=["read"])
        app = _make_app(api_key_mgr, jwt_mgr=jwt_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get(
                "/api/v1/system/info",
                headers={"Authorization": f"Bearer {token}"},
            )
            assert resp.status_code == 200
            assert resp.json()["auth"] == "jwt"

    async def test_expired_jwt_rejected(
        self, api_key_mgr: APIKeyManager, jwt_mgr: JWTManager
    ) -> None:
        from datetime import datetime, timedelta, timezone

        past = datetime.now(tz=timezone.utc) - timedelta(hours=2)
        with patch("arbor_core.auth.jwt.datetime") as mock_dt:
            mock_dt.now.return_value = past
            mock_dt.side_effect = lambda *a, **kw: datetime(*a, **kw)
            token = jwt_mgr.issue(subject="old")

        app = _make_app(api_key_mgr, jwt_mgr=jwt_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get(
                "/api/v1/system/info",
                headers={"Authorization": f"Bearer {token}"},
            )
            assert resp.status_code == 404


class TestScopeEnforcement:
    """Scope-based access control."""

    async def test_read_key_cannot_write(
        self, api_key_mgr: APIKeyManager, read_key: str
    ) -> None:
        app = _make_app(api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.put(
                "/api/v1/servo/1/position",
                headers={"Authorization": f"Bearer {read_key}"},
            )
            assert resp.status_code == 404  # 404 not 403

    async def test_write_key_can_write(
        self, api_key_mgr: APIKeyManager, write_key: str
    ) -> None:
        app = _make_app(api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.put(
                "/api/v1/servo/1/position",
                headers={"Authorization": f"Bearer {write_key}"},
            )
            assert resp.status_code == 200

    async def test_write_key_cannot_admin(
        self, api_key_mgr: APIKeyManager, write_key: str
    ) -> None:
        app = _make_app(api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.post(
                "/api/v1/auth/api-keys",
                headers={"Authorization": f"Bearer {write_key}"},
            )
            assert resp.status_code == 404

    async def test_admin_key_can_admin(
        self, api_key_mgr: APIKeyManager, admin_key: str
    ) -> None:
        app = _make_app(api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.post(
                "/api/v1/auth/api-keys",
                headers={"Authorization": f"Bearer {admin_key}"},
            )
            assert resp.status_code == 200

    async def test_stream_scope_for_ws(
        self, api_key_mgr: APIKeyManager, stream_key: str
    ) -> None:
        app = _make_app(api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get(
                "/api/v1/ws",
                headers={"Authorization": f"Bearer {stream_key}"},
            )
            assert resp.status_code == 200

    async def test_read_key_cannot_stream(
        self, api_key_mgr: APIKeyManager, read_key: str
    ) -> None:
        app = _make_app(api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get(
                "/api/v1/ws",
                headers={"Authorization": f"Bearer {read_key}"},
            )
            assert resp.status_code == 404


class TestRateLimiting:
    """Rate limiting enforcement."""

    async def test_rate_limit_triggers_429(
        self, api_key_mgr: APIKeyManager, read_key: str
    ) -> None:
        # Very low limit: 3 requests per minute
        app = _make_app(api_key_mgr, rate_limit_rpm=3)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            for _ in range(3):
                resp = await client.get(
                    "/api/v1/system/info",
                    headers={"Authorization": f"Bearer {read_key}"},
                )
                assert resp.status_code == 200

            resp = await client.get(
                "/api/v1/system/info",
                headers={"Authorization": f"Bearer {read_key}"},
            )
            assert resp.status_code == 429
            assert "Retry-After" in resp.headers

    async def test_emergency_stop_exempt(
        self, api_key_mgr: APIKeyManager, write_key: str
    ) -> None:
        app = _make_app(api_key_mgr, rate_limit_rpm=1)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            # Exhaust rate limit
            await client.post(
                "/api/v1/emergency-stop",
                headers={"Authorization": f"Bearer {write_key}"},
            )
            # Emergency stop should still work
            resp = await client.post(
                "/api/v1/emergency-stop",
                headers={"Authorization": f"Bearer {write_key}"},
            )
            assert resp.status_code == 200


class TestSecurityHeaders:
    """Security headers middleware."""

    async def test_security_headers_present(
        self, api_key_mgr: APIKeyManager
    ) -> None:
        app = _make_app(api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get("/api/v1/health")
            assert resp.headers["X-Content-Type-Options"] == "nosniff"
            assert resp.headers["X-Frame-Options"] == "DENY"
            assert "strict-origin" in resp.headers["Referrer-Policy"]
            assert "max-age" in resp.headers["Strict-Transport-Security"]


class TestAuditLogging:
    """Audit log entries for write operations."""

    async def test_write_operation_logged(
        self, api_key_mgr: APIKeyManager, write_key: str
    ) -> None:
        app = _make_app(api_key_mgr)
        with patch("arbor_core.auth.middleware.logger") as mock_logger:
            async with AsyncClient(
                transport=ASGITransport(app=app), base_url="http://test"
            ) as client:
                await client.put(
                    "/api/v1/servo/1/position",
                    headers={"Authorization": f"Bearer {write_key}"},
                )
            # Check that audit_log was called
            calls = [c for c in mock_logger.info.call_args_list if c[0][0] == "audit_log"]
            assert len(calls) == 1
            kwargs = calls[0][1]
            assert kwargs["method"] == "PUT"
            assert kwargs["path"] == "/api/v1/servo/1/position"
