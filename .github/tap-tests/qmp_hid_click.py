#!/usr/bin/env python3
"""Send a physical QEMU USB-tablet click into the Windows 11 guest.

This is a test-only harness. No Win32 PostMessage or synthetic mouse_event
is used for the acceptance click.
"""
import json
import pathlib
import socket
import sys
import time

root = pathlib.Path(sys.argv[1])
shared = root / "shared"
proof = json.loads((shared / "win11-native-x64-proof.json").read_text(encoding="utf-8-sig"))
assert proof["Stage"] == "QMP_HID_READY", proof.get("Stage")
target = proof["QmpHidTarget"]
assert target["HitClass"] == "OpenShell.CStartButton", target
x, y = int(target["X"]), int(target["Y"])
width, height = int(target["Width"]), int(target["Height"])
assert 0 <= x < width and 0 <= y < height and width > 1 and height > 1, target

socket_path = root / "storage" / "qmp.sock"
for _ in range(100):
    if socket_path.is_socket():
        break
    time.sleep(0.2)
else:
    raise RuntimeError("QMP socket unavailable; cannot certify hardware click")

go = shared / "qmp-hid-go.txt"
done = shared / "qmp-hid-done.json"
go.unlink(missing_ok=True)
done.unlink(missing_ok=True)

class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(12)
        self.sock.connect(str(path))
        self.stream = self.sock.makefile("rb")
        self.receive()  # greeting
        self.call("qmp_capabilities")

    def receive(self):
        while True:
            line = self.stream.readline()
            if not line:
                raise RuntimeError("QMP disconnected")
            reply = json.loads(line)
            if "event" in reply:
                continue
            if "error" in reply:
                raise RuntimeError("QMP error: " + json.dumps(reply["error"]))
            return reply

    def call(self, name, arguments=None):
        command = {"execute": name}
        if arguments is not None:
            command["arguments"] = arguments
        self.sock.sendall((json.dumps(command) + "\n").encode())
        response = self.receive()
        if "return" not in response:
            raise RuntimeError("Unexpected QMP response: " + repr(response))
        return response["return"]

    def close(self):
        self.stream.close()
        self.sock.close()

qmp = Qmp(socket_path)
try:
    # Guest starts 50ms menu/focus sampling as soon as this marker appears.
    go.write_text(json.dumps({"X": x, "Y": y}), encoding="utf-8")
    time.sleep(0.15)
    qmp.call("input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": round(x * 32767 / (width - 1))}},
        {"type": "abs", "data": {"axis": "y", "value": round(y * 32767 / (height - 1))}}
    ]})
    time.sleep(0.2)
    qmp.call("input-send-event", {"events": [
        {"type": "btn", "data": {"down": True, "button": "left"}}
    ]})
    time.sleep(0.15)
    qmp.call("input-send-event", {"events": [
        {"type": "btn", "data": {"down": False, "button": "left"}}
    ]})
    done.write_text(json.dumps({"Sent": True, "X": x, "Y": y,
                                "Input": "QMP usb-tablet left button down/up"}),
                    encoding="utf-8")
    print("QMP_HID_CLICK_SENT: virtual USB-tablet input at", x, y)
except Exception as exc:
    done.write_text(json.dumps({"Sent": False, "Error": str(exc)}), encoding="utf-8")
    print("QMP_HID_CLICK_FAILED:", exc, file=sys.stderr)
finally:
    qmp.close()
