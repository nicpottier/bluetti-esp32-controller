# PlatformIO pre-build script: stamps the firmware with its git version.
#   FW_VERSION  shown on screen: "1.2.0" for a tagged release, "1.2.0-DEV" otherwise
#   FW_BUILD    full `git describe`, printed on the serial console at boot
import subprocess

Import("env")


def git(*args):
    try:
        return subprocess.check_output(["git", *args], cwd=env["PROJECT_DIR"], text=True,
                                       stderr=subprocess.DEVNULL).strip()
    except Exception:
        return ""


build = git("describe", "--tags", "--always", "--dirty", "--match", "v[0-9]*") or "unknown"
tag = git("describe", "--tags", "--abbrev=0", "--match", "v[0-9]*")
release = tag and build == tag  # exactly on a tag, no local changes
version = (tag[1:] if tag else "0.0.0") + ("" if release else "-DEV")

env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version)),
                       ("FW_BUILD", env.StringifyMacro(build))])
print(f"Firmware version {version} ({build})")
