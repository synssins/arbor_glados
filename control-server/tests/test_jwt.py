"""
Tests for JWT RS256 auth (C06).

Covers:
- RSA keypair generation
- Token issuance and verification
- Expired token rejection
- Tampered token rejection
- Algorithm enforcement (RS256 only)
- Required claims validation
- from_key_files factory
"""

from __future__ import annotations

from pathlib import Path
from unittest.mock import patch

import jwt as pyjwt
import pytest

from arbor_core.auth.jwt import JWTError, JWTManager, generate_keypair


@pytest.fixture()
def key_dir(tmp_path: Path) -> Path:
    """Temporary directory for key files."""
    return tmp_path / "keys"


@pytest.fixture()
def keypair(key_dir: Path) -> tuple[Path, Path]:
    """Generate a fresh keypair and return (private_path, public_path)."""
    priv = key_dir / "private.pem"
    pub = key_dir / "public.pem"
    generate_keypair(priv, pub)
    return priv, pub


@pytest.fixture()
def manager(keypair: tuple[Path, Path]) -> JWTManager:
    """JWTManager loaded from generated keypair."""
    priv, pub = keypair
    return JWTManager.from_key_files(priv, pub, expiry_minutes=60)


class TestKeypairGeneration:
    """RSA keypair generation."""

    def test_generates_files(self, key_dir: Path) -> None:
        priv = key_dir / "private.pem"
        pub = key_dir / "public.pem"
        generate_keypair(priv, pub)
        assert priv.exists()
        assert pub.exists()
        assert b"PRIVATE KEY" in priv.read_bytes()
        assert b"PUBLIC KEY" in pub.read_bytes()

    def test_refuses_overwrite(self, keypair: tuple[Path, Path]) -> None:
        priv, pub = keypair
        with pytest.raises(FileExistsError):
            generate_keypair(priv, pub)

    def test_rejects_small_key_size(self, key_dir: Path) -> None:
        with pytest.raises(ValueError, match="2048"):
            generate_keypair(key_dir / "p.pem", key_dir / "u.pem", key_size=1024)

    def test_creates_parent_dirs(self, tmp_path: Path) -> None:
        deep = tmp_path / "a" / "b" / "c"
        priv = deep / "private.pem"
        pub = deep / "public.pem"
        generate_keypair(priv, pub)
        assert priv.exists()


class TestTokenIssuance:
    """JWT token issuance."""

    def test_issue_returns_string(self, manager: JWTManager) -> None:
        token = manager.issue(subject="user@test.com")
        assert isinstance(token, str)
        assert len(token) > 0

    def test_issue_with_scopes(self, manager: JWTManager) -> None:
        token = manager.issue(subject="admin", scopes=["read", "write", "admin"])
        payload = manager.verify(token)
        assert payload["scopes"] == ["read", "write", "admin"]

    def test_issue_with_extra_claims(self, manager: JWTManager) -> None:
        token = manager.issue(subject="user", extra_claims={"role": "operator"})
        payload = manager.verify(token)
        assert payload["role"] == "operator"

    def test_each_token_unique_jti(self, manager: JWTManager) -> None:
        t1 = manager.issue(subject="a")
        t2 = manager.issue(subject="a")
        p1 = manager.verify(t1)
        p2 = manager.verify(t2)
        assert p1["jti"] != p2["jti"]


class TestTokenVerification:
    """JWT token verification."""

    def test_verify_valid_token(self, manager: JWTManager) -> None:
        token = manager.issue(subject="user@test.com")
        payload = manager.verify(token)
        assert payload["sub"] == "user@test.com"
        assert payload["iss"] == "arbor"
        assert "exp" in payload
        assert "iat" in payload
        assert "jti" in payload

    def test_verify_expired_token(self, manager: JWTManager) -> None:
        # Create a manager with 0 minute expiry won't work (min 1), so mock time
        short_mgr = JWTManager(
            private_key_pem=manager._private_key_pem,
            public_key_pem=manager._public_key_pem,
            expiry_minutes=1,
        )
        token = short_mgr.issue(subject="expired")
        # Manually decode to tamper with exp
        # Instead, use a negative expiry by patching datetime
        from datetime import datetime, timedelta, timezone

        past = datetime.now(tz=timezone.utc) - timedelta(hours=2)
        with patch("arbor_core.auth.jwt.datetime") as mock_dt:
            mock_dt.now.return_value = past
            mock_dt.side_effect = lambda *a, **kw: datetime(*a, **kw)
            expired_token = short_mgr.issue(subject="old")

        with pytest.raises(JWTError, match="expired"):
            manager.verify(expired_token)

    def test_verify_tampered_token(self, manager: JWTManager) -> None:
        token = manager.issue(subject="clean")
        # Flip a character in the signature
        tampered = token[:-2] + ("A" if token[-2] != "A" else "B") + token[-1]
        with pytest.raises(JWTError):
            manager.verify(tampered)

    def test_verify_wrong_issuer(self, keypair: tuple[Path, Path]) -> None:
        priv, pub = keypair
        mgr_a = JWTManager.from_key_files(priv, pub, issuer="other-system")
        mgr_b = JWTManager.from_key_files(priv, pub, issuer="arbor")
        token = mgr_a.issue(subject="user")
        with pytest.raises(JWTError, match="issuer"):
            mgr_b.verify(token)

    def test_rejects_hs256_token(self, manager: JWTManager) -> None:
        """Ensure HS256 tokens are rejected even if they have valid claims."""
        fake_token = pyjwt.encode(
            {"sub": "hacker", "iss": "arbor", "iat": 0, "exp": 9999999999, "jti": "x"},
            "fake-secret",
            algorithm="HS256",
        )
        with pytest.raises(JWTError):
            manager.verify(fake_token)

    def test_garbage_input(self, manager: JWTManager) -> None:
        with pytest.raises(JWTError):
            manager.verify("not.a.jwt")

    def test_empty_string(self, manager: JWTManager) -> None:
        with pytest.raises(JWTError):
            manager.verify("")


class TestFromKeyFiles:
    """Factory method from_key_files."""

    def test_loads_from_disk(self, keypair: tuple[Path, Path]) -> None:
        priv, pub = keypair
        mgr = JWTManager.from_key_files(priv, pub, expiry_minutes=30)
        token = mgr.issue(subject="disk-test")
        payload = mgr.verify(token)
        assert payload["sub"] == "disk-test"
