"""``python -m mynah`` — the entry point the login service uses.

The LaunchAgent prefers a renamed Python binary so the process shows as
"mynah" rather than "Python", and that path can only run a module, not a
console script. So this file is load-bearing: without it the agent exits 1 at
every launch.
"""

from mynah.cli import main

if __name__ == "__main__":
    raise SystemExit(main())
