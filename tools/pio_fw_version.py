# PlatformIO pre-script (docs.platformio.org "Advanced Scripting"): puts the
# git short SHA of the tree being built into VIZZZ_GIT_SHA, with "+dirty" when
# the working tree has uncommitted changes, so /status "fw" names the build
# running on a node (audit 2026-10-15 1-L4). No git -> "nogit".
import subprocess

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)


def _git(*args):
    try:
        return subprocess.check_output(["git", *args], cwd=env["PROJECT_DIR"],  # noqa: F821
                                       stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return ""


sha = _git("rev-parse", "--short", "HEAD") or "nogit"
if sha != "nogit" and _git("status", "--porcelain", "--untracked-files=no"):
    sha += "+dirty"
env.Append(CPPDEFINES=[("VIZZZ_GIT_SHA", env.StringifyMacro(sha))])  # noqa: F821
