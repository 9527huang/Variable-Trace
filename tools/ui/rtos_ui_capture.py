"""Capture the task view and compare it with the reference on record.

Self-contained on purpose: it launches the program, opens the fixture project
over the MCP port, walks to the task view, and grabs the same region every time.
Nothing here lives in a scratch directory, because a baseline nobody can
reproduce is a picture rather than a test.

    python tools/ui/rtos_ui_capture.py --update     write the reference
    python tools/ui/rtos_ui_capture.py              compare with it

It is a sample rather than a gate. Two things are against a pixel comparison
here: the capture is of this machine's fonts and scale, and the task view is
live - the tasks, the numbers and the cursor positions are different every run.
So what the number is good for is its own history: a run that reports far more
than the ones before it has something in it that changed, and the two pictures
side by side say what. What it catches is the change nobody meant: a column off
the end, a legend that stopped drawing, text running into a number.

The screen this was written on is 2560x1440 at 150 per cent, and the clicks are
in physical pixels. The tab widths move with the interface language, so the run
opens the variable view first - which is also what gives the plot cursors their
positions - and then comes back to the task view.
"""
import argparse
import ctypes
import ctypes.wintypes
import json
import os
import subprocess
import sys
import time
import urllib.request

from PIL import Image, ImageChops, ImageGrab, ImageStat

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REFERENCES = os.path.join(ROOT, "test", "ui")
APP = os.path.join(ROOT, "build", "Variable-Trace.exe")
PROJECT = os.path.join(ROOT, "analysis", "tmp", "rtos_verify.mcuvproj")
LATEST = os.path.join(ROOT, "analysis", "tmp", "rtos_ui_latest.png")

# Physical pixels: the task view's panel, and the places the run has to click.
REGION = (0, 380, 1010, 1150)
VAR_VIEW_TAB = (100, 57)
TASK_VIEW_TAB = (284, 57)
A_TASK_ROW = (120, 421)

MCP = "http://127.0.0.1:7807/mcp"
user32 = ctypes.windll.user32


def mcp_tool(name, arguments=None):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": name, "arguments": arguments or {}}}).encode()

    # The system proxy would take a request for 127.0.0.1 and fail with it.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    with opener.open(urllib.request.Request(MCP, data=body, headers={"Content-Type": "application/json"}), timeout=30) as response:
        payload = json.load(response)

    content = payload.get("result", {}).get("content", [])
    text = content[0]["text"] if content else "{}"

    return json.loads(text)


def click(point):
    user32.SetForegroundWindow(find_window())
    time.sleep(0.3)
    user32.SetCursorPos(point[0], point[1])
    time.sleep(0.2)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.8)


def find_window():
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def visit(hwnd, param):
        if user32.IsWindowVisible(hwnd):
            length = user32.GetWindowTextLengthW(hwnd)

            if length:
                buffer = ctypes.create_unicode_buffer(length + 1)
                user32.GetWindowTextW(hwnd, buffer, length + 1)

                if buffer.value.startswith("Variable-Trace"):
                    found.append(hwnd)

        return True

    user32.EnumWindows(visit, None)

    if not found:
        raise SystemExit("the program is not running under the expected title")

    return found[0]


def grab(theme):
    config = os.path.join(os.environ["APPDATA"], "Variable-Trace", "global.json")

    with open(config, encoding="utf-8") as handle:
        saved = handle.read()

    app = None

    try:
        settings = json.loads(saved)
        settings["interface"]["theme"] = theme

        with open(config, "w", encoding="utf-8") as handle:
            json.dump(settings, handle, ensure_ascii=False, indent=2)

        app = subprocess.Popen([APP], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(16)

        mcp_tool("open_project", {"path": PROJECT})
        time.sleep(1)
        mcp_tool("start_acquisition", {})
        time.sleep(10)

        # The variable view first: its plots place the cursors the timeline
        # follows, and the cursors are placed the first time the plot draws.
        click(VAR_VIEW_TAB)
        time.sleep(2)
        click(TASK_VIEW_TAB)
        time.sleep(2)
        click(A_TASK_ROW)

        image = ImageGrab.grab().convert("RGB").crop(REGION)

        if not os.path.isdir(os.path.join(ROOT, "analysis", "tmp")):
            os.makedirs(os.path.join(ROOT, "analysis", "tmp"), exist_ok=True)

        image.save(LATEST)

        return image
    finally:
        if app is not None:
            app.terminate()

            try:
                app.wait(timeout=20)
            except subprocess.TimeoutExpired:
                app.kill()

        with open(config, "w", encoding="utf-8") as handle:
            handle.write(saved)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--update", action="store_true")
    parser.add_argument("--tolerance", type=float, default=30.0)
    arguments = parser.parse_args()

    os.makedirs(REFERENCES, exist_ok=True)

    name = "rtos_task_view_dark.png"
    image = grab("dark")

    if arguments.update:
        image.save(os.path.join(REFERENCES, name))
        print("wrote", os.path.join(REFERENCES, name))

        return 0

    reference = Image.open(os.path.join(REFERENCES, name)).convert("RGB")

    if reference.size != image.size:
        print("size changed: %s -> %s" % (reference.size, image.size))
        print("FAIL")

        return 1

    difference = ImageStat.Stat(ImageChops.difference(reference, image)).mean
    mean = sum(difference) / 3.0

    print("mean difference %5.2f (channels %.2f %.2f %.2f, allowed %.1f)" % (mean, difference[0], difference[1], difference[2], arguments.tolerance))

    if mean > arguments.tolerance:
        print("FAIL - look at analysis/tmp/rtos_ui_latest.png against", os.path.join(REFERENCES, name))

        return 1

    print("PASS")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
