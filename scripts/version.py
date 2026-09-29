"""Bake the version, the git hash and the build date into the firmware.

Typing a version into a header means it is wrong the first time somebody
forgets to bump it, and a log naming the wrong commit is worse than one naming
none. So it comes out of git, on every build.

A working tree with uncommitted changes to tracked files produces a hash with
`-dirty` on it. Untracked files do not count — they are not in the build.

The build date is to the minute, in UTC. Two builds from the same dirty tree are
otherwise indistinguishable, and a measurement that cannot say which binary
produced it is a measurement nobody can repeat. To the minute rather than the
second so a rebuild within one does not churn the whole binary.
"""

import datetime
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

built = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%MZ")

env.Append(  # noqa: F821
    CPPDEFINES=[
        ("ZW_FW_VERSION", env.StringifyMacro(version)),  # noqa: F821
        ("ZW_GIT_HASH", env.StringifyMacro(git_hash)),  # noqa: F821
        ("ZW_GIT_DIRTY", dirty),
        ("ZW_BUILD_DATE", env.StringifyMacro(built)),  # noqa: F821
    ]
)

print("version: %s  git: %s  built: %s" % (version, git_hash, built))
