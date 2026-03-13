"""
Tests for SQLite backend (C16).

Covers:
- Database initialization and schema migration
- API key repository: save, get, list, revoke, delete
- Audit log repository: append, query with filters, count
- Schema version tracking
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

from arbor_core.auth.api_keys import APIKeyRecord, APIKeyScope
from arbor_core.db.engine import Database
from arbor_core.db.repositories import APIKeyRepository, AuditLogRepository


@pytest.fixture()
def db() -> Database:
    """In-memory SQLite database for testing."""
    d = Database(path=Path(":memory:"))
    d.initialize()
    yield d  # type: ignore[misc]
    d.close()


@pytest.fixture()
def key_repo(db: Database) -> APIKeyRepository:
    return APIKeyRepository(db)


@pytest.fixture()
def audit_repo(db: Database) -> AuditLogRepository:
    return AuditLogRepository(db)


def _make_record(
    key_id: str = "test-id",
    name: str = "test-key",
    scopes: list[APIKeyScope] | None = None,
) -> APIKeyRecord:
    """Helper to create a test API key record."""
    return APIKeyRecord(
        id=key_id,
        name=name,
        key_hash="$argon2id$v=19$m=65536,t=3,p=4$fakehash",
        prefix="abcd1234",
        scopes=scopes or [APIKeyScope.READ],
        created_at=datetime.now(tz=timezone.utc),
    )


class TestDatabaseInit:
    """Database initialization and migrations."""

    def test_initialize_creates_tables(self) -> None:
        db = Database(path=Path(":memory:"))
        db.initialize()
        conn = db.connection
        # Check tables exist
        cursor = conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"
        )
        tables = {row[0] for row in cursor.fetchall()}
        assert "api_keys" in tables
        assert "audit_log" in tables
        assert "schema_version" in tables
        db.close()

    def test_schema_version_set(self, db: Database) -> None:
        cursor = db.connection.execute("SELECT MAX(version) FROM schema_version")
        assert cursor.fetchone()[0] == 1

    def test_double_initialize_idempotent(self) -> None:
        db = Database(path=Path(":memory:"))
        db.initialize()
        db.initialize()  # Should not fail
        cursor = db.connection.execute("SELECT COUNT(*) FROM schema_version")
        assert cursor.fetchone()[0] == 1  # Only one version record
        db.close()

    def test_file_based_db(self, tmp_path: Path) -> None:
        db_path = tmp_path / "test.db"
        db = Database(path=db_path)
        db.initialize()
        assert db_path.exists()
        db.close()

    def test_connection_before_init_raises(self) -> None:
        db = Database(path=Path(":memory:"))
        with pytest.raises(RuntimeError, match="not initialized"):
            _ = db.connection


class TestAPIKeyRepository:
    """API key CRUD operations."""

    def test_save_and_get(self, key_repo: APIKeyRepository) -> None:
        record = _make_record()
        key_repo.save(record)
        fetched = key_repo.get_by_id("test-id")
        assert fetched is not None
        assert fetched.id == "test-id"
        assert fetched.name == "test-key"
        assert fetched.prefix == "abcd1234"

    def test_get_nonexistent(self, key_repo: APIKeyRepository) -> None:
        assert key_repo.get_by_id("nope") is None

    def test_list_all(self, key_repo: APIKeyRepository) -> None:
        key_repo.save(_make_record("id-1", "key-1"))
        key_repo.save(_make_record("id-2", "key-2"))
        all_keys = key_repo.list_all()
        assert len(all_keys) == 2

    def test_list_active_excludes_revoked(self, key_repo: APIKeyRepository) -> None:
        key_repo.save(_make_record("id-1", "active"))
        key_repo.save(_make_record("id-2", "revoked"))
        key_repo.revoke("id-2")
        active = key_repo.list_active()
        assert len(active) == 1
        assert active[0].name == "active"

    def test_revoke(self, key_repo: APIKeyRepository) -> None:
        key_repo.save(_make_record("id-1"))
        assert key_repo.revoke("id-1") is True
        fetched = key_repo.get_by_id("id-1")
        assert fetched is not None
        assert fetched.revoked is True
        assert fetched.revoked_at is not None

    def test_revoke_nonexistent(self, key_repo: APIKeyRepository) -> None:
        assert key_repo.revoke("nope") is False

    def test_delete(self, key_repo: APIKeyRepository) -> None:
        key_repo.save(_make_record("id-1"))
        assert key_repo.delete("id-1") is True
        assert key_repo.get_by_id("id-1") is None

    def test_scopes_roundtrip(self, key_repo: APIKeyRepository) -> None:
        record = _make_record(
            scopes=[APIKeyScope.READ, APIKeyScope.WRITE, APIKeyScope.STREAM]
        )
        key_repo.save(record)
        fetched = key_repo.get_by_id("test-id")
        assert fetched is not None
        assert set(fetched.scopes) == {APIKeyScope.READ, APIKeyScope.WRITE, APIKeyScope.STREAM}

    def test_upsert_on_save(self, key_repo: APIKeyRepository) -> None:
        key_repo.save(_make_record("id-1", "original"))
        key_repo.save(_make_record("id-1", "updated"))
        fetched = key_repo.get_by_id("id-1")
        assert fetched is not None
        assert fetched.name == "updated"


class TestAuditLogRepository:
    """Audit log operations."""

    def test_append_and_query(self, audit_repo: AuditLogRepository) -> None:
        audit_repo.append(
            method="PUT",
            path="/api/v1/servo/1/position",
            status_code=200,
            auth_type="api_key",
            subject="admin",
            client_ip="192.168.1.10",
            key_id="key-123",
        )
        entries = audit_repo.query(limit=10)
        assert len(entries) == 1
        assert entries[0]["method"] == "PUT"
        assert entries[0]["client_ip"] == "192.168.1.10"

    def test_count(self, audit_repo: AuditLogRepository) -> None:
        for i in range(5):
            audit_repo.append(
                method="POST",
                path=f"/api/v1/action/{i}",
                status_code=200,
                auth_type="jwt",
                subject="user",
                client_ip="10.0.0.1",
            )
        assert audit_repo.count() == 5

    def test_query_with_limit_offset(self, audit_repo: AuditLogRepository) -> None:
        for i in range(10):
            audit_repo.append(
                method="GET",
                path=f"/api/v1/item/{i}",
                status_code=200,
                auth_type="api_key",
                subject="reader",
                client_ip="10.0.0.1",
            )
        page = audit_repo.query(limit=3, offset=2)
        assert len(page) == 3

    def test_query_by_key_id(self, audit_repo: AuditLogRepository) -> None:
        audit_repo.append(
            method="PUT", path="/a", status_code=200,
            auth_type="api_key", subject="a", client_ip="1.1.1.1", key_id="key-A",
        )
        audit_repo.append(
            method="PUT", path="/b", status_code=200,
            auth_type="api_key", subject="b", client_ip="2.2.2.2", key_id="key-B",
        )
        results = audit_repo.query(key_id="key-A")
        assert len(results) == 1
        assert results[0]["key_id"] == "key-A"

    def test_query_by_time_range(self, audit_repo: AuditLogRepository) -> None:
        # Insert with default timestamp (now — SQLite datetime('now') is UTC without tz)
        audit_repo.append(
            method="POST", path="/test", status_code=201,
            auth_type="jwt", subject="user", client_ip="10.0.0.1",
        )
        # Query with a wide range that includes now.
        # Use naive UTC to match SQLite's datetime('now') format.
        now = datetime.now(tz=timezone.utc).replace(tzinfo=None)
        results = audit_repo.query(
            since=now - timedelta(minutes=5),
            until=now + timedelta(minutes=5),
        )
        assert len(results) == 1

    def test_empty_query(self, audit_repo: AuditLogRepository) -> None:
        assert audit_repo.query() == []
        assert audit_repo.count() == 0
