"""Smoke test: myra-ui runs one scripted turn headless and exits cleanly.

CUI_SCREENSHOT makes the app save a PNG after N frames and quit, so no input is
needed. Exits 77 (ctest: skipped) where no window can open.
Usage: smoke.py MYRA_UI [SCREENSHOT]
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ui = sys.argv[1]
if sys.platform.startswith("linux") and not os.environ.get("DISPLAY"):
    sys.exit(77)

with tempfile.TemporaryDirectory() as tmp:
    work = Path(tmp, "work")
    work.mkdir()
    (work / "notes.txt").write_text("hello\n")
    shot = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(tmp, "shot.png")
    mock = subprocess.Popen([sys.executable, "-I", str(Path(__file__).with_name("mock_server.py"))],
                            stdout=subprocess.PIPE, text=True)
    try:
        port = mock.stdout.readline().strip()
        env = {**os.environ, "XDG_STATE_HOME": str(Path(tmp, "state")),
               "LOCAL_BASE_URL": f"http://127.0.0.1:{port}/v1",
               "CUI_SCREENSHOT": str(shot), "CUI_SCREENSHOT_FRAMES": "300"}
        p = subprocess.run([ui, "-P", "local", "--permissions", "all", "-C", str(work),
                            "What is in this folder?"], env=env, capture_output=True, text=True,
                           timeout=60, check=False)
    finally:
        mock.kill()
    assert p.returncode == 0, p.stderr
    assert shot.exists(), p.stderr
    assert (work / "notes.txt").read_text() == "hello, world\n", "the edit did not run"
    print("smoke: ok")
