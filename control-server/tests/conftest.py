"""
Pytest configuration and fixtures for Arbor tests.

Provides common fixtures for:
- FastAPI test client
- Mock configurations
- Database sessions (when implemented)
"""

from collections.abc import AsyncGenerator
from typing import TYPE_CHECKING

import pytest
from fastapi.testclient import TestClient
from httpx import ASGITransport, AsyncClient

from arbor_core.app import create_app

if TYPE_CHECKING:
    from fastapi import FastAPI


@pytest.fixture
def app() -> "FastAPI":
    """Create a test FastAPI application instance."""
    return create_app(config=None)


@pytest.fixture
def client(app: "FastAPI") -> TestClient:
    """Create a synchronous test client."""
    return TestClient(app)


@pytest.fixture
async def async_client(app: "FastAPI") -> AsyncGenerator[AsyncClient, None]:
    """Create an async test client for async endpoint testing."""
    async with AsyncClient(
        transport=ASGITransport(app=app),
        base_url="http://test",
    ) as ac:
        yield ac


# Additional fixtures will be added as needed:
# - mock_config: Mock ArborConfig for testing
# - db_session: Database session for integration tests
# - mock_node: Mock ESP32 node for bridge testing
