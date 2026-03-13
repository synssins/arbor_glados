"""
Database repositories — API key store and audit log.

Thin wrappers around SQLite that match the in-memory interfaces
from auth/api_keys.py, enabling persistent storage.

Task: C16
"""

from __future__ import annotations

import json
from datetime import datetime, timezone
from typing import Any

import structlog

from arbor_core.auth.api_keys import APIKeyRecord, APIKeyScope
from arbor_core.db.engine import Database

logger = structlog.get_logger(__name__)


class APIKeyRepository:
    """
    Persistent storage for API key records.

    Wraps SQLite operations for the api_keys table.
    """

    def __init__(self, db: Database) -> None:
        self._db = db

    def save(self, record: APIKeyRecord) -> None:
        """Insert or update an API key record."""
        conn = self._db.connection
        conn.execute(
            """\
            INSERT OR REPLACE INTO api_keys
                (id, name, key_hash, prefix, scopes, created_at,
                 revoked, revoked_at, rate_limit_rpm, ip_allowlist)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                record.id,
                record.name,
                record.key_hash,
                record.prefix,
                json.dumps([s.value for s in record.scopes]),
                record.created_at.isoformat(),
                1 if record.revoked else 0,
                record.revoked_at.isoformat() if record.revoked_at else None,
                record.rate_limit_rpm,
                json.dumps(record.ip_allowlist),
            ),
        )
        conn.commit()

    def get_by_id(self, key_id: str) -> APIKeyRecord | None:
        """Retrieve an API key record by ID."""
        conn = self._db.connection
        cursor = conn.execute("SELECT * FROM api_keys WHERE id = ?", (key_id,))
        row = cursor.fetchone()
        if row is None:
            return None
        return self._row_to_record(row)

    def list_all(self) -> list[APIKeyRecord]:
        """List all API key records."""
        conn = self._db.connection
        cursor = conn.execute("SELECT * FROM api_keys ORDER BY created_at DESC")
        return [self._row_to_record(row) for row in cursor.fetchall()]

    def list_active(self) -> list[APIKeyRecord]:
        """List only non-revoked API key records."""
        conn = self._db.connection
        cursor = conn.execute(
            "SELECT * FROM api_keys WHERE revoked = 0 ORDER BY created_at DESC"
        )
        return [self._row_to_record(row) for row in cursor.fetchall()]

    def revoke(self, key_id: str) -> bool:
        """Mark a key as revoked. Returns True if the key was found."""
        conn = self._db.connection
        now = datetime.now(tz=timezone.utc).isoformat()
        cursor = conn.execute(
            "UPDATE api_keys SET revoked = 1, revoked_at = ? WHERE id = ? AND revoked = 0",
            (now, key_id),
        )
        conn.commit()
        return cursor.rowcount > 0

    def delete(self, key_id: str) -> bool:
        """Permanently delete a key record. Returns True if the key was found."""
        conn = self._db.connection
        cursor = conn.execute("DELETE FROM api_keys WHERE id = ?", (key_id,))
        conn.commit()
        return cursor.rowcount > 0

    @staticmethod
    def _row_to_record(row: Any) -> APIKeyRecord:
        """Convert a SQLite Row to an APIKeyRecord."""
        scopes_raw = json.loads(row["scopes"])
        return APIKeyRecord(
            id=row["id"],
            name=row["name"],
            key_hash=row["key_hash"],
            prefix=row["prefix"],
            scopes=[APIKeyScope(s) for s in scopes_raw],
            created_at=datetime.fromisoformat(row["created_at"]),
            revoked=bool(row["revoked"]),
            revoked_at=(
                datetime.fromisoformat(row["revoked_at"]) if row["revoked_at"] else None
            ),
            rate_limit_rpm=row["rate_limit_rpm"],
            ip_allowlist=json.loads(row["ip_allowlist"]),
        )


class AuditLogRepository:
    """
    Append-only audit log storage.

    Writes are immediate; reads support filtering by time range and key.
    The audit log cannot be deleted via API (read-only to non-admin).
    """

    def __init__(self, db: Database) -> None:
        self._db = db

    def append(
        self,
        method: str,
        path: str,
        status_code: int,
        auth_type: str,
        subject: str,
        client_ip: str,
        key_id: str | None = None,
        params_summary: str | None = None,
    ) -> None:
        """Write an audit log entry."""
        conn = self._db.connection
        conn.execute(
            """\
            INSERT INTO audit_log
                (method, path, status_code, auth_type, subject, key_id,
                 client_ip, params_summary)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (method, path, status_code, auth_type, subject, key_id, client_ip, params_summary),
        )
        conn.commit()

    def query(
        self,
        limit: int = 100,
        offset: int = 0,
        key_id: str | None = None,
        since: datetime | None = None,
        until: datetime | None = None,
    ) -> list[dict[str, Any]]:
        """
        Query audit log entries.

        Args:
            limit: Max entries to return.
            offset: Pagination offset.
            key_id: Filter by API key ID.
            since: Filter entries after this timestamp.
            until: Filter entries before this timestamp.

        Returns:
            List of audit log entry dicts.
        """
        conn = self._db.connection
        conditions: list[str] = []
        params: list[Any] = []

        if key_id:
            conditions.append("key_id = ?")
            params.append(key_id)
        if since:
            conditions.append("timestamp >= ?")
            # SQLite datetime('now') uses space separator, not ISO 'T'
            params.append(since.strftime("%Y-%m-%d %H:%M:%S"))
        if until:
            conditions.append("timestamp <= ?")
            params.append(until.strftime("%Y-%m-%d %H:%M:%S"))

        where = f"WHERE {' AND '.join(conditions)}" if conditions else ""
        query = f"SELECT * FROM audit_log {where} ORDER BY id DESC LIMIT ? OFFSET ?"  # noqa: S608
        params.extend([limit, offset])

        cursor = conn.execute(query, params)
        return [dict(row) for row in cursor.fetchall()]

    def count(self) -> int:
        """Return the total number of audit log entries."""
        conn = self._db.connection
        cursor = conn.execute("SELECT COUNT(*) FROM audit_log")
        row = cursor.fetchone()
        return row[0] if row else 0
