"""
Tests for auth API endpoints (C15).

Covers:
- POST /api/v1/auth/api-keys — create key (admin only)
- GET  /api/v1/auth/api-keys — list keys (admin only)
- DELETE /api/v1/auth/api-keys/{id} — revoke key (admin only)
- POST /api/v1/auth/logout
- Non-admin access returns 404 (not 403)
"""

from __future__ import annotations

from typing import Any

import pytest
from fastapi import FastAPI
from httpx import ASGITransport, AsyncClient

from arbor_core.api.v1.auth_routes import auth_router
from arbor_core.auth.api_keys import APIKeyManager, APIKeyScope
from arbor_core.auth.middleware import AuthMiddleware, AuthState


def _make_app(api_key_mgr: APIKeyManager) -> FastAPI:
    """Build a test app with auth routes and middleware-like auth injection."""
    app = FastAPI()

    # Mount auth routes under /api/v1
    from fastapi import APIRouter

    v1 = APIRouter(prefix="/api/v1")
    v1.include_router(auth_router)
    app.include_router(v1)

    # Store API key manager in app state
    app.state.api_key_manager = api_key_mgr

    return app


def _auth_headers(key: str) -> dict[str, str]:
    return {"Authorization": f"Bearer {key}"}


@pytest.fixture()
def api_key_mgr() -> APIKeyManager:
    return APIKeyManager(min_key_length=32)


@pytest.fixture()
def admin_key(api_key_mgr: APIKeyManager) -> str:
    result = api_key_mgr.create_key(name="admin", scopes=[APIKeyScope.ADMIN])
    return result.plaintext_key


@pytest.fixture()
def read_key(api_key_mgr: APIKeyManager) -> str:
    result = api_key_mgr.create_key(name="reader", scopes=[APIKeyScope.READ])
    return result.plaintext_key


def _inject_auth(app: FastAPI, api_key_mgr: APIKeyManager) -> FastAPI:
    """Add the real auth middleware to the app."""
    app.add_middleware(
        AuthMiddleware,
        api_key_manager=api_key_mgr,
        jwt_manager=None,
        default_rate_limit_rpm=1000,
    )
    return app


class TestCreateAPIKey:
    """POST /api/v1/auth/api-keys."""

    async def test_admin_can_create_key(
        self, api_key_mgr: APIKeyManager, admin_key: str
    ) -> None:
        app = _inject_auth(_make_app(api_key_mgr), api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.post(
                "/api/v1/auth/api-keys",
                json={"name": "new-key", "scopes": ["read", "write"]},
                headers=_auth_headers(admin_key),
            )
            assert resp.status_code == 201
            data = resp.json()
            assert "plaintext_key" in data
            assert data["name"] == "new-key"
            assert set(data["scopes"]) == {"read", "write"}

    async def test_non_admin_gets_404(
        self, api_key_mgr: APIKeyManager, read_key: str
    ) -> None:
        app = _inject_auth(_make_app(api_key_mgr), api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.post(
                "/api/v1/auth/api-keys",
                json={"name": "sneaky", "scopes": ["admin"]},
                headers=_auth_headers(read_key),
            )
            assert resp.status_code == 404

    async def test_unauthenticated_gets_404(self, api_key_mgr: APIKeyManager) -> None:
        app = _inject_auth(_make_app(api_key_mgr), api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.post(
                "/api/v1/auth/api-keys",
                json={"name": "anon", "scopes": ["read"]},
            )
            assert resp.status_code == 404


class TestListAPIKeys:
    """GET /api/v1/auth/api-keys."""

    async def test_admin_can_list(
        self, api_key_mgr: APIKeyManager, admin_key: str
    ) -> None:
        app = _inject_auth(_make_app(api_key_mgr), api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get(
                "/api/v1/auth/api-keys",
                headers=_auth_headers(admin_key),
            )
            assert resp.status_code == 200
            data = resp.json()
            assert "keys" in data
            # At least the admin key and read key from fixtures
            assert len(data["keys"]) >= 1

    async def test_list_hides_hashes(
        self, api_key_mgr: APIKeyManager, admin_key: str
    ) -> None:
        app = _inject_auth(_make_app(api_key_mgr), api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.get(
                "/api/v1/auth/api-keys",
                headers=_auth_headers(admin_key),
            )
            for key in resp.json()["keys"]:
                assert "key_hash" not in key


class TestRevokeAPIKey:
    """DELETE /api/v1/auth/api-keys/{id}."""

    async def test_admin_can_revoke(
        self, api_key_mgr: APIKeyManager, admin_key: str
    ) -> None:
        # Create a key to revoke
        target = api_key_mgr.create_key(name="target", scopes=[APIKeyScope.READ])

        app = _inject_auth(_make_app(api_key_mgr), api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.delete(
                f"/api/v1/auth/api-keys/{target.id}",
                headers=_auth_headers(admin_key),
            )
            assert resp.status_code == 200
            assert resp.json()["detail"] == "Key revoked"

    async def test_revoke_nonexistent_returns_404(
        self, api_key_mgr: APIKeyManager, admin_key: str
    ) -> None:
        app = _inject_auth(_make_app(api_key_mgr), api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.delete(
                "/api/v1/auth/api-keys/nonexistent-id",
                headers=_auth_headers(admin_key),
            )
            assert resp.status_code == 404


class TestLogout:
    """POST /api/v1/auth/logout."""

    async def test_logout_returns_200(
        self, api_key_mgr: APIKeyManager, admin_key: str
    ) -> None:
        app = _inject_auth(_make_app(api_key_mgr), api_key_mgr)
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test"
        ) as client:
            resp = await client.post(
                "/api/v1/auth/logout",
                headers=_auth_headers(admin_key),
            )
            assert resp.status_code == 200
