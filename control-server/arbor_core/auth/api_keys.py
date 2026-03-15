"""
API key management — create, list, revoke.

Keys are generated as cryptographically random tokens, then hashed
with Argon2id before storage. The plaintext key is returned exactly
once at creation time and never stored.

Scopes: read, write, admin, stream
Per ARBOR_PROJECT_PLAN.md Section 7 and SECURITY.md.

Task: C05
Dependency: argon2-cffi (see decisions/DECISION_REQUEST_1.md)
"""

from __future__ import annotations

import secrets
import uuid
from datetime import datetime, timezone
from enum import Enum
from typing import Any

import structlog
from pydantic import BaseModel, Field

try:
    from argon2 import PasswordHasher
    from argon2.exceptions import VerifyMismatchError

    _ARGON2_AVAILABLE = True
except ImportError:
    _ARGON2_AVAILABLE = False

logger = structlog.get_logger(__name__)


class APIKeyScope(str, Enum):
    """Scopes that can be assigned to an API key."""

    READ = "read"
    WRITE = "write"
    ADMIN = "admin"
    STREAM = "stream"


class APIKeyRecord(BaseModel):
    """
    Stored record for an API key.

    The plaintext key is NEVER stored. Only the Argon2id hash is persisted.
    """

    id: str = Field(description="Unique key identifier (UUID).")
    name: str = Field(description="Human-readable label for the key.")
    key_hash: str = Field(description="Argon2id hash of the API key.")
    prefix: str = Field(
        description="First 8 chars of the key, for identification in logs/UI."
    )
    scopes: list[APIKeyScope] = Field(description="Granted scopes.")
    created_at: datetime = Field(description="Creation timestamp (UTC).")
    revoked: bool = Field(default=False, description="Whether this key has been revoked.")
    revoked_at: datetime | None = Field(default=None, description="Revocation timestamp.")
    rate_limit_rpm: int | None = Field(
        default=None, description="Per-key rate limit override (requests per minute)."
    )
    ip_allowlist: list[str] = Field(
        default_factory=list,
        description="Optional IP allowlist. Empty means all IPs allowed.",
    )


class APIKeyCreateResult(BaseModel):
    """Result returned when a new API key is created."""

    id: str
    name: str
    plaintext_key: str = Field(
        description="The API key in plaintext. Returned ONCE at creation time only."
    )
    prefix: str
    scopes: list[APIKeyScope]
    created_at: datetime


class APIKeyManager:
    """
    Manages API key creation, verification, and revocation.

    Keys are stored in-memory for now. C16 (SQLite backend) will add
    persistent storage.

    Usage::

        mgr = APIKeyManager(min_key_length=32)
        result = mgr.create_key(name="glados", scopes=[APIKeyScope.WRITE, APIKeyScope.STREAM])
        # result.plaintext_key is the only time you see it

        record = mgr.verify_key(result.plaintext_key)
        # record is APIKeyRecord if valid, None if invalid/revoked
    """

    def __init__(self, min_key_length: int = 32) -> None:
        """
        Args:
            min_key_length: Minimum length for generated keys.
                            Comes from config.security.api_key_min_length.
        """
        if not _ARGON2_AVAILABLE:
            msg = (
                "argon2-cffi is required for API key management. "
                "See decisions/DECISION_REQUEST_1.md. "
                "Install with: pip install argon2-cffi"
            )
            raise ImportError(msg)

        self._min_key_length = min_key_length
        self._hasher = PasswordHasher()
        self._keys: dict[str, APIKeyRecord] = {}

    @property
    def key_count(self) -> int:
        """Number of active (non-revoked) keys. Zero means provisioning mode."""
        return sum(1 for k in self._keys.values() if not k.revoked)

    def create_key(
        self,
        name: str,
        scopes: list[APIKeyScope],
        rate_limit_rpm: int | None = None,
        ip_allowlist: list[str] | None = None,
    ) -> APIKeyCreateResult:
        """
        Create a new API key.

        Args:
            name: Human-readable label.
            scopes: List of scopes to grant.
            rate_limit_rpm: Optional per-key rate limit override.
            ip_allowlist: Optional IP allowlist.

        Returns:
            APIKeyCreateResult with the plaintext key (shown once).
        """
        key_id = str(uuid.uuid4())
        plaintext_key = secrets.token_urlsafe(self._min_key_length)
        prefix = plaintext_key[:8]
        key_hash = self._hasher.hash(plaintext_key)
        now = datetime.now(tz=timezone.utc)

        record = APIKeyRecord(
            id=key_id,
            name=name,
            key_hash=key_hash,
            prefix=prefix,
            scopes=scopes,
            created_at=now,
            rate_limit_rpm=rate_limit_rpm,
            ip_allowlist=ip_allowlist or [],
        )
        self._keys[key_id] = record

        logger.info(
            "api_key_created",
            key_id=key_id,
            name=name,
            prefix=prefix,
            scopes=[s.value for s in scopes],
        )

        return APIKeyCreateResult(
            id=key_id,
            name=name,
            plaintext_key=plaintext_key,
            prefix=prefix,
            scopes=scopes,
            created_at=now,
        )

    def verify_key(self, plaintext_key: str) -> APIKeyRecord | None:
        """
        Verify an API key and return its record.

        Checks every stored hash (necessary since we can't look up
        by plaintext). Returns None if no match or if the key is revoked.

        Args:
            plaintext_key: The API key to verify.

        Returns:
            APIKeyRecord if valid and not revoked, None otherwise.
        """
        for record in self._keys.values():
            if record.revoked:
                continue
            try:
                self._hasher.verify(record.key_hash, plaintext_key)
                # Re-hash if needed (argon2 parameter upgrades)
                if self._hasher.check_needs_rehash(record.key_hash):
                    record.key_hash = self._hasher.hash(plaintext_key)
                    logger.info("api_key_rehashed", key_id=record.id)
                return record
            except VerifyMismatchError:
                continue

        logger.warning("api_key_verify_failed", prefix=plaintext_key[:8] if plaintext_key else "")
        return None

    def list_keys(self) -> list[dict[str, Any]]:
        """
        List all API keys (without hashes).

        Returns:
            List of key metadata dicts (id, name, prefix, scopes, status).
        """
        return [
            {
                "id": record.id,
                "name": record.name,
                "prefix": record.prefix,
                "scopes": [s.value for s in record.scopes],
                "created_at": record.created_at.isoformat(),
                "revoked": record.revoked,
                "revoked_at": record.revoked_at.isoformat() if record.revoked_at else None,
            }
            for record in self._keys.values()
        ]

    def revoke_key(self, key_id: str, *, allow_last: bool = False) -> bool:
        """
        Revoke an API key by ID.

        The check-and-revoke is atomic (single synchronous method) to
        prevent race conditions between key_count check and revocation.
        When allow_last is False (default), revoking the last active key
        is refused to prevent re-entering provisioning mode.

        Args:
            key_id: The UUID of the key to revoke.
            allow_last: If True, allow revoking even the last key.

        Returns:
            True if the key was found and revoked, False if not found.

        Raises:
            ValueError: If this is the last active key and allow_last is False.
        """
        record = self._keys.get(key_id)
        if record is None:
            logger.warning("api_key_revoke_not_found", key_id=key_id)
            return False

        if record.revoked:
            logger.info("api_key_already_revoked", key_id=key_id)
            return True

        # Guard: prevent revoking the last active key (atomic with revoke)
        if not allow_last and self.key_count <= 1:
            logger.warning("api_key_revoke_blocked_last", key_id=key_id)
            raise ValueError("Cannot revoke the last active access code")

        record.revoked = True
        record.revoked_at = datetime.now(tz=timezone.utc)
        logger.info("api_key_revoked", key_id=key_id, name=record.name)
        return True

    def get_key_by_id(self, key_id: str) -> APIKeyRecord | None:
        """Get a key record by its ID."""
        return self._keys.get(key_id)

    def has_scope(self, record: APIKeyRecord, required_scope: APIKeyScope) -> bool:
        """
        Check if a key record has the required scope.

        Admin scope implies all other scopes.

        Args:
            record: The API key record.
            required_scope: The scope to check for.

        Returns:
            True if the key has the required scope.
        """
        if APIKeyScope.ADMIN in record.scopes:
            return True
        return required_scope in record.scopes
