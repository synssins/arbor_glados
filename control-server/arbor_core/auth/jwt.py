"""
JWT authentication — RS256 keypair generation, token issuance, and verification.

Algorithm is locked to RS256 per ARBOR_PROJECT_PLAN.md Section 13.
The algorithm is ALWAYS set server-side — never derived from the incoming token.

Task: C06
Dependencies: PyJWT, cryptography (see decisions/DECISION_REQUEST_1.md)
"""

from __future__ import annotations

import uuid
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

import jwt
import structlog
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa

logger = structlog.get_logger(__name__)

_ALGORITHM = "RS256"


class JWTError(Exception):
    """Raised on JWT operation failures."""


def generate_keypair(
    private_key_path: Path,
    public_key_path: Path,
    key_size: int = 2048,
) -> None:
    """
    Generate an RSA keypair and write PEM files to disk.

    Does NOT overwrite existing files. If either file exists,
    raises FileExistsError.

    Args:
        private_key_path: Where to write the private key PEM.
        public_key_path: Where to write the public key PEM.
        key_size: RSA key size in bits. Minimum 2048.

    Raises:
        FileExistsError: If either key file already exists.
        ValueError: If key_size < 2048.
    """
    if key_size < 2048:  # noqa: PLR2004
        msg = f"RSA key size must be >= 2048, got {key_size}"
        raise ValueError(msg)

    if private_key_path.exists():
        msg = f"Private key file already exists: {private_key_path}"
        raise FileExistsError(msg)
    if public_key_path.exists():
        msg = f"Public key file already exists: {public_key_path}"
        raise FileExistsError(msg)

    private_key = rsa.generate_private_key(
        public_exponent=65537,
        key_size=key_size,
    )

    # Write private key
    private_key_path.parent.mkdir(parents=True, exist_ok=True)
    private_key_path.write_bytes(
        private_key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption(),
        )
    )

    # Write public key
    public_key_path.parent.mkdir(parents=True, exist_ok=True)
    public_key_path.write_bytes(
        private_key.public_key().public_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PublicFormat.SubjectPublicKeyInfo,
        )
    )

    logger.info(
        "jwt_keypair_generated",
        private_key_path=str(private_key_path),
        public_key_path=str(public_key_path),
        key_size=key_size,
    )


class JWTManager:
    """
    Issues and verifies RS256 JWT tokens for WebUI sessions.

    The algorithm is always RS256. It is NEVER read from the token header.

    Usage::

        mgr = JWTManager(
            private_key_pem=private_key_bytes,
            public_key_pem=public_key_bytes,
            expiry_minutes=60,
        )
        token = mgr.issue(subject="user@example.com", scopes=["read", "write"])
        payload = mgr.verify(token)
    """

    def __init__(
        self,
        private_key_pem: bytes,
        public_key_pem: bytes,
        expiry_minutes: int = 60,
        issuer: str = "arbor",
    ) -> None:
        """
        Args:
            private_key_pem: RSA private key in PEM format (for signing).
            public_key_pem: RSA public key in PEM format (for verification).
            expiry_minutes: Token lifetime in minutes. From config.security.jwt_expiry_minutes.
            issuer: JWT issuer claim.
        """
        self._private_key_pem = private_key_pem
        self._public_key_pem = public_key_pem
        self._expiry_minutes = expiry_minutes
        self._issuer = issuer

    @classmethod
    def from_key_files(
        cls,
        private_key_path: Path,
        public_key_path: Path,
        expiry_minutes: int = 60,
        issuer: str = "arbor",
    ) -> JWTManager:
        """
        Create a JWTManager by loading key files from disk.

        Args:
            private_key_path: Path to RSA private key PEM.
            public_key_path: Path to RSA public key PEM.
            expiry_minutes: Token lifetime.
            issuer: JWT issuer claim.

        Returns:
            Configured JWTManager instance.
        """
        private_pem = private_key_path.read_bytes()
        public_pem = public_key_path.read_bytes()
        return cls(
            private_key_pem=private_pem,
            public_key_pem=public_pem,
            expiry_minutes=expiry_minutes,
            issuer=issuer,
        )

    def issue(
        self,
        subject: str,
        scopes: list[str] | None = None,
        extra_claims: dict[str, Any] | None = None,
    ) -> str:
        """
        Issue a signed JWT token.

        Args:
            subject: The subject claim (user identifier or API key ID).
            scopes: List of scope strings embedded in the token.
            extra_claims: Additional claims to include.

        Returns:
            Encoded JWT string.
        """
        now = datetime.now(tz=timezone.utc)
        payload: dict[str, Any] = {
            "sub": subject,
            "iss": self._issuer,
            "iat": now,
            "exp": now + timedelta(minutes=self._expiry_minutes),
            "jti": str(uuid.uuid4()),
            "scopes": scopes or [],
        }
        if extra_claims:
            payload.update(extra_claims)

        token: str = jwt.encode(payload, self._private_key_pem, algorithm=_ALGORITHM)

        logger.info(
            "jwt_issued",
            subject=subject,
            scopes=scopes or [],
            jti=payload["jti"],
        )
        return token

    def verify(self, token: str) -> dict[str, Any]:
        """
        Verify and decode a JWT token.

        The algorithm is ALWAYS RS256 — never derived from the token header.

        Args:
            token: The encoded JWT string.

        Returns:
            Decoded payload dictionary.

        Raises:
            JWTError: If the token is invalid, expired, or tampered.
        """
        try:
            payload: dict[str, Any] = jwt.decode(
                token,
                self._public_key_pem,
                algorithms=[_ALGORITHM],
                issuer=self._issuer,
                options={
                    "require": ["sub", "iss", "iat", "exp", "jti"],
                },
            )
        except jwt.ExpiredSignatureError as exc:
            msg = "Token has expired"
            raise JWTError(msg) from exc
        except jwt.InvalidIssuerError as exc:
            msg = "Invalid token issuer"
            raise JWTError(msg) from exc
        except jwt.InvalidTokenError as exc:
            msg = f"Invalid token: {exc}"
            raise JWTError(msg) from exc

        return payload
