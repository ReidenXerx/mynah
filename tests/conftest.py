"""pytest config — ensure the whiz package is importable from the repo root."""

from __future__ import annotations

import sys
from pathlib import Path

# Insert repo root so `import whiz` works without installing.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))