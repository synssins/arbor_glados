# Arbor Control Server

The Control Server is the central orchestration layer of Arbor. It provides:

- REST API for hardware control
- WebSocket for real-time state updates
- Plugin system for extensible hardware support
- Multi-node management
- Authentication and authorization

## Development Setup

```bash
# Create virtual environment
python -m venv .venv
source .venv/bin/activate  # Linux/macOS
# or: .venv\Scripts\activate  # Windows

# Install in development mode
pip install -e ".[dev]"

# Run tests
pytest

# Run linting
ruff check .
black --check .
mypy arbor_core

# Start development server
python -m arbor_core.main
```

## Project Structure

```
control-server/
├── arbor_core/
│   ├── __init__.py         # Package entry
│   ├── app.py              # FastAPI application factory
│   ├── main.py             # CLI entry point
│   ├── api/                # API routers
│   │   └── v1/             # API v1 endpoints
│   ├── auth/               # Authentication (C05-C07)
│   ├── bridges/            # Hardware node bridges (C08-C09)
│   ├── config/             # Configuration (C02-C03)
│   └── plugins/            # Plugin system (C04)
├── tests/                  # Test suite
└── pyproject.toml          # Project configuration
```

## Configuration

All configuration is loaded from YAML files. See `../config/arbor.example.yaml` for the full schema.

**Zero hardcoded values** - every configurable parameter must come from config.

## API Documentation

When the server is running, API documentation is available at:
- Swagger UI: `http://localhost:8000/api/docs`
- ReDoc: `http://localhost:8000/api/redoc`
- OpenAPI JSON: `http://localhost:8000/api/openapi.json`
