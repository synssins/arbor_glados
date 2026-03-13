"""
Arbor authentication package.

Handles API key management, JWT tokens, and auth middleware.

Security requirements (from SECURITY.md):
- API keys stored hashed (Argon2id)
- JWT algorithm explicitly RS256
- Unauthorized returns 404 (not 403) to prevent enumeration
- Rate limiting per API key

Implementation tasks:
- C05: API key management - create (Argon2id hash), list, revoke  [DONE]
- C06: JWT auth - RS256 keypair gen, issue, verify  [DONE]
- C07: Auth middleware - scope enforcement, rate limiting, audit log
"""

from arbor_core.auth.api_keys import (
    APIKeyCreateResult,
    APIKeyManager,
    APIKeyRecord,
    APIKeyScope,
)
from arbor_core.auth.jwt import JWTError, JWTManager, generate_keypair

__all__ = [
    "APIKeyCreateResult",
    "APIKeyManager",
    "APIKeyRecord",
    "APIKeyScope",
    "JWTError",
    "JWTManager",
    "generate_keypair",
]
