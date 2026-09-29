"""Bake the version and the git hash into the firmware at build time.

Typing a version into a header means it is wrong the first time somebody
forgets to bump it, and a log that names the wrong commit is worse than one
that names none. So it comes from git, on every build.

A working tree with uncommitted changes to tracked files produces a hash with
`-dirty` on it. Untracked files do not count — they are not in the build.
"""

import subprocess

Import("env")  # noqa: F821  — injected by PlatformIO


def git(*args):
    try:
        out = subprocess.check_output(("git",) + args, stderr=subprocess.DEVNULL)
        return out.decode("utf-8", "replace").strip()
    except Exception:
        return ""


commit = git("rev-parse", "--short=7", "HEAD")

if not commit:
    # No git, or not a checkout. Say so rather than inventing something that
    # looks like a hash.
    version, git_hash, dirty = "unknown", "unknown", 0
else:
    dirty = 1 if git("status", "--porcelain", "--untracked-files=no") else 0
    git_hash = commit + ("-dirty" if dirty else "")
    # No tags yet, and `git describe` fails rather than guessing. That is the
    # honest answer until something is released.
    version = git("describe", "--tags", "--dirty=-dirty") or (
        "0.0.0-dev" + ("-dirty" if dirty else "")
    )

env.Append(  # noqa: F821
    CPPDEFINES=[
        ("ZW_FW_VERSION", env.StringifyMacro(version)),  # noqa: F821
        ("ZW_GIT_HASH", env.StringifyMacro(git_hash)),  # noqa: F821
        ("ZW_GIT_DIRTY", dirty),
    ]
)

print("version: %s  git: %s" % (version, git_hash))
