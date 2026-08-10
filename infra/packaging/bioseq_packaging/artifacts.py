"""! @file
@brief Shared operations for native-package artifact files.
"""

from __future__ import annotations

import hashlib
from pathlib import Path


_READ_CHUNK_BYTES = 1024 * 1024


def sha256_file(path: Path) -> str:
    """@brief Compute the lowercase SHA-256 digest of an artifact file.

    @param path Artifact path to read.
    @return The file digest as 64 lowercase hexadecimal characters.
    @throws OSError If the artifact cannot be opened or read.
    """

    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(_READ_CHUNK_BYTES), b""):
            digest.update(chunk)
    return digest.hexdigest()
