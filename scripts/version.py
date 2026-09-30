"""Bake where the firmware came from into the firmware.

Typing a version into a header means it is wrong the first time somebody
forgets to bump it, and a log naming the wrong commit is worse than one naming
none. So it comes out of git, on every build.

A working tree with uncommitted changes to tracked files produces a hash with
`-dirty` on it. Untracked files do not count — they are not in the build.

The build date is to the minute, in UTC. Two builds from the same dirty tree are
otherwise indistinguishable, and a measurement that cannot say which binary
produced it is a measurement nobody can repeat. To the minute rather than the
second so a rebuild within one does not churn the whole binary.

The repository goes in too. A hash on its own names a commit in some repository;
with the slug it names a commit, and a recording that outlives somebody's memory
of which project it came from is still readable.

There is no flash timestamp, because the board has no clock to make one with.
With `task flash` and `task ota` the build happens in the same command as the
upload, so the build date *is* when it was written — and where they differ, the
sink's `recv_at` on the session event says when the image started talking.
"""

import datetime
import re
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

# owner/repo rather than the whole URL: it is what a person recognises, and it
# survives the remote being moved between https and ssh.
repo = git("remote", "get-url", "origin")
repo = re.sub(r"^(https://[^/]+/|git@[^:]+:)", "", repo)
repo = re.sub(r"\.git$", "", repo) or "unknown"

env.Append(  # noqa: F821
    CPPDEFINES=[
        ("ZW_FW_VERSION", env.StringifyMacro(version)),  # noqa: F821
        ("ZW_GIT_HASH", env.StringifyMacro(git_hash)),  # noqa: F821
        ("ZW_GIT_DIRTY", dirty),
        ("ZW_BUILD_DATE", env.StringifyMacro(built)),  # noqa: F821
        ("ZW_GIT_REPO", env.StringifyMacro(repo)),  # noqa: F821
    ]
)

print("version: %s  git: %s  built: %s  repo: %s" % (version, git_hash, built, repo))
