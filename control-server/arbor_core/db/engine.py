"""
SQLite database engine — connection management and schema migrations.

Uses Python's built-in sqlite3 module (no additional dependencies).
All operations are async-safe by running in a thread executor.

Task: C16
"""

from __future__ import annotations

import sqlite3
from pathlib import Path

import structlog

logger = structlog.get_logger(__name__)

# Schema version tracking
_CURRENT_SCHEMA_VERSION = 1

_SCHEMA_V1 = """\
-- Schema version tracking
CREATE TABLE IF NOT EXISTS schema_version (
    version INTEGER NOT NULL,
    applied_at TEXT NOT NULL DEFAULT (datetime('now'))
);

-- API key storage
CREATE TABLE IF NOT EXISTS api_keys (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    key_hash TEXT NOT NULL,
    prefix TEXT NOT NULL,
    scopes TEXT NOT NULL,         -- JSON array of scope strings
    created_at TEXT NOT NULL,
    revoked INTEGER NOT NULL DEFAULT 0,
    revoked_at TEXT,
    rate_limit_rpm INTEGER,
    ip_allowlist TEXT NOT NULL DEFAULT '[]'  -- JSON array of IP strings
);

CREATE INDEX IF NOT EXISTS idx_api_keys_prefix ON api_keys(prefix);
CREATE INDEX IF NOT EXISTS idx_api_keys_revoked ON api_keys(revoked);

-- Audit log (append-only)
CREATE TABLE IF NOT EXISTS audit_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT NOT NULL DEFAULT (datetime('now')),
    method TEXT NOT NULL,
    path TEXT NOT NULL,
    status_code INTEGER NOT NULL,
    auth_type TEXT NOT NULL,
    subject TEXT NOT NULL,
    key_id TEXT,
    client_ip TEXT NOT NULL,
    params_summary TEXT
);

CREATE INDEX IF NOT EXISTS idx_audit_log_timestamp ON audit_log(timestamp);
CREATE INDEX IF NOT EXISTS idx_audit_log_key_id ON audit_log(key_id);
"""


class Database:
    """
    SQLite database manager with schema migration support.

    Usage::

        db = Database(path=Path("arbor.db"))
        db.initialize()
        conn = db.connection
        ...
        db.close()
    """

    def __init__(self, path: Path) -> None:
        """
        Args:
            path: Path to the SQLite database file.
                  Use ":memory:" (as a string converted to Path) for testing.
        """
        self._path = path
        self._conn: sqlite3.Connection | None = None

    @property
    def connection(self) -> sqlite3.Connection:
        """Get the active database connection."""
        if self._conn is None:
            msg = "Database not initialized. Call initialize() first."
            raise RuntimeError(msg)
        return self._conn

    def initialize(self) -> None:
        """
        Open the database connection and apply migrations.

        Creates the database file if it doesn't exist.
        Applies any pending schema migrations.
        """
        db_str = str(self._path)
        # Support :memory: for testing
        if db_str == ":memory:":
            self._conn = sqlite3.connect(":memory:")
        else:
            self._path.parent.mkdir(parents=True, exist_ok=True)
            self._conn = sqlite3.connect(db_str)

        self._conn.row_factory = sqlite3.Row
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute("PRAGMA foreign_keys=ON")

        self._migrate()
        logger.info("database_initialized", path=db_str)

    def close(self) -> None:
        """Close the database connection."""
        if self._conn is not None:
            self._conn.close()
            self._conn = None
            logger.info("database_closed")

    def _migrate(self) -> None:
        """Apply schema migrations."""
        conn = self.connection
        current_version = self._get_schema_version()

        if current_version < 1:
            logger.info("database_migrating", from_version=current_version, to_version=1)
            conn.executescript(_SCHEMA_V1)
            conn.execute(
                "INSERT INTO schema_version (version) VALUES (?)",
                (_CURRENT_SCHEMA_VERSION,),
            )
            conn.commit()
            logger.info("database_migration_complete", version=1)

    def _get_schema_version(self) -> int:
        """Get the current schema version from the database."""
        conn = self.connection
        try:
            cursor = conn.execute(
                "SELECT MAX(version) FROM schema_version"
            )
            row = cursor.fetchone()
            return row[0] if row and row[0] is not None else 0
        except sqlite3.OperationalError:
            # Table doesn't exist yet
            return 0
