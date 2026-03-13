"""
Tests for the FastAPI application.

Basic smoke tests to verify the application skeleton works.
"""

from fastapi.testclient import TestClient

from arbor_core import __version__, create_app


class TestAppCreation:
    """Test application factory."""

    def test_create_app_returns_fastapi(self) -> None:
        """Application factory returns FastAPI instance."""
        app = create_app()
        assert app is not None
        assert app.title == "Arbor Control Server"

    def test_version_is_set(self) -> None:
        """Package version is defined."""
        assert __version__ == "0.1.0"


class TestHealthEndpoint:
    """Test the health check endpoint."""

    def test_health_returns_200(self, client: TestClient) -> None:
        """Health endpoint returns 200 OK."""
        response = client.get("/api/v1/health")
        assert response.status_code == 200

    def test_health_returns_status(self, client: TestClient) -> None:
        """Health endpoint returns status field."""
        response = client.get("/api/v1/health")
        data = response.json()
        assert data["status"] == "healthy"

    def test_health_returns_version(self, client: TestClient) -> None:
        """Health endpoint includes version."""
        response = client.get("/api/v1/health")
        data = response.json()
        assert "version" in data


class TestAPIDocumentation:
    """Test API documentation endpoints."""

    def test_openapi_available(self, client: TestClient) -> None:
        """OpenAPI spec is served."""
        response = client.get("/api/openapi.json")
        assert response.status_code == 200
        data = response.json()
        assert data["info"]["title"] == "Arbor Control Server"

    def test_swagger_ui_available(self, client: TestClient) -> None:
        """Swagger UI is accessible."""
        response = client.get("/api/docs")
        assert response.status_code == 200

    def test_redoc_available(self, client: TestClient) -> None:
        """ReDoc is accessible."""
        response = client.get("/api/redoc")
        assert response.status_code == 200
