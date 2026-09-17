"""pytest config — make `import mynah` work, and keep tmp paths bindable."""

from __future__ import annotations

import re
import sys
import uuid
from pathlib import Path

import pytest

# Insert repo root so `import mynah` works without installing.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))


if sys.platform == "darwin":
    # sockaddr_un holds 104 bytes on macOS, and bind()/connect() refuse any
    # path that does not fit — with "AF_UNIX path too long", an error that has
    # nothing to do with the code under test. pytest's default macOS basetemp
    # spends ~90 of them before the test name is even appended:
    #
    #   /private/var/folders/<xx>/<26 random>/T/pytest-of-<user>/pytest-<n>/…
    #
    # so every test that binds a unix socket under tmp_path — the
    # control-socket suite, the socket-driven providers — fails on a stock
    # `python -m pytest` and passes with `--basetemp=/tmp/…`, which is exactly
    # the kind of environment-dependent failure this suite exists to not have.
    #
    # Hand those tests a short symlink instead: the path string the kernel
    # sees fits, while the files themselves stay inside pytest's managed tree,
    # where retention and failure inspection still find them. Linux's
    # /tmp-based basetemp already fits its 108-byte limit, so there the stock
    # fixture is left alone.
    @pytest.fixture
    def tmp_path(tmp_path_factory: pytest.TempPathFactory, request: pytest.FixtureRequest) -> Path:
        """The stock tmp_path — sanitised name, numbered, factory-managed —
        reached through a path short enough to bind a unix socket in."""
        name = re.sub(r"[\W]", "_", request.node.name)[:30]
        real = tmp_path_factory.mktemp(name, numbered=True)
        short = Path("/tmp") / f"mynah-pytest-{uuid.uuid4().hex[:12]}"
        short.symlink_to(real, target_is_directory=True)
        request.addfinalizer(lambda: short.unlink(missing_ok=True))
        return short