"""
Tests for API key management (C05).

Covers:
- Key creation with proper hashing
- Key verification (valid, invalid, revoked)
- Key listing (no hashes exposed)
- Key revocation
- Scope checking (admin implies all)
"""

from __future__ import annotations

import pytest

from arbor_core.auth.api_keys import APIKeyCreateResult, APIKeyManager, APIKeyScope


@pytest.fixture()
def manager() -> APIKeyManager:
    return APIKeyManager(min_key_length=32)


class TestKeyCreation:
    """API key creation."""

    def test_create_returns_result(self, manager: APIKeyManager) -> None:
        result = manager.create_key(name="test", scopes=[APIKeyScope.READ])
        assert isinstance(result, APIKeyCreateResult)
        assert result.name == "test"
        assert len(result.plaintext_key) >= 32
        assert result.prefix == result.plaintext_key[:8]

    def test_create_multiple_unique(self, manager: APIKeyManager) -> None:
        r1 = manager.create_key(name="a", scopes=[APIKeyScope.READ])
        r2 = manager.create_key(name="b", scopes=[APIKeyScope.WRITE])
        assert r1.id != r2.id
        assert r1.plaintext_key != r2.plaintext_key

    def test_scopes_preserved(self, manager: APIKeyManager) -> None:
        scopes = [APIKeyScope.READ, APIKeyScope.STREAM]
        result = manager.create_key(name="multi", scopes=scopes)
        assert set(result.scopes) == set(scopes)


class TestKeyVerification:
    """API key verification."""

    def test_verify_valid_key(self, manager: APIKeyManager) -> None:
        result = manager.create_key(name="valid", scopes=[APIKeyScope.READ])
        record = manager.verify_key(result.plaintext_key)
        assert record is not None
        assert record.id == result.id
        assert record.name == "valid"

    def test_verify_invalid_key(self, manager: APIKeyManager) -> None:
        manager.create_key(name="real", scopes=[APIKeyScope.READ])
        record = manager.verify_key("totally-bogus-key-that-does-not-exist")
        assert record is None

    def test_verify_revoked_key(self, manager: APIKeyManager) -> None:
        result = manager.create_key(name="revokable", scopes=[APIKeyScope.WRITE])
        manager.revoke_key(result.id)
        record = manager.verify_key(result.plaintext_key)
        assert record is None

    def test_verify_empty_string(self, manager: APIKeyManager) -> None:
        record = manager.verify_key("")
        assert record is None


class TestKeyListing:
    """Key listing — must not expose hashes."""

    def test_list_empty(self, manager: APIKeyManager) -> None:
        assert manager.list_keys() == []

    def test_list_returns_metadata(self, manager: APIKeyManager) -> None:
        manager.create_key(name="listed", scopes=[APIKeyScope.READ])
        keys = manager.list_keys()
        assert len(keys) == 1
        assert keys[0]["name"] == "listed"
        assert "key_hash" not in keys[0]
        assert "prefix" in keys[0]

    def test_list_multiple(self, manager: APIKeyManager) -> None:
        manager.create_key(name="a", scopes=[APIKeyScope.READ])
        manager.create_key(name="b", scopes=[APIKeyScope.WRITE])
        assert len(manager.list_keys()) == 2


class TestKeyRevocation:
    """Key revocation."""

    def test_revoke_existing(self, manager: APIKeyManager) -> None:
        result = manager.create_key(name="doomed", scopes=[APIKeyScope.READ])
        assert manager.revoke_key(result.id) is True

    def test_revoke_nonexistent(self, manager: APIKeyManager) -> None:
        assert manager.revoke_key("no-such-id") is False

    def test_revoke_idempotent(self, manager: APIKeyManager) -> None:
        result = manager.create_key(name="double", scopes=[APIKeyScope.READ])
        assert manager.revoke_key(result.id) is True
        assert manager.revoke_key(result.id) is True  # already revoked

    def test_revoked_key_shows_in_list(self, manager: APIKeyManager) -> None:
        result = manager.create_key(name="revoked", scopes=[APIKeyScope.READ])
        manager.revoke_key(result.id)
        keys = manager.list_keys()
        assert keys[0]["revoked"] is True
        assert keys[0]["revoked_at"] is not None


class TestScopeChecking:
    """Scope enforcement."""

    def test_has_matching_scope(self, manager: APIKeyManager) -> None:
        result = manager.create_key(name="reader", scopes=[APIKeyScope.READ])
        record = manager.verify_key(result.plaintext_key)
        assert record is not None
        assert manager.has_scope(record, APIKeyScope.READ) is True
        assert manager.has_scope(record, APIKeyScope.WRITE) is False

    def test_admin_implies_all(self, manager: APIKeyManager) -> None:
        result = manager.create_key(name="admin", scopes=[APIKeyScope.ADMIN])
        record = manager.verify_key(result.plaintext_key)
        assert record is not None
        assert manager.has_scope(record, APIKeyScope.READ) is True
        assert manager.has_scope(record, APIKeyScope.WRITE) is True
        assert manager.has_scope(record, APIKeyScope.STREAM) is True
        assert manager.has_scope(record, APIKeyScope.ADMIN) is True
