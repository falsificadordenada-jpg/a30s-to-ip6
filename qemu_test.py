#!/usr/bin/env python3
"""Boot an image in QEMU (virt, cortex-a53, 4 GiB so 0xEC000000 is RAM), dump the
fake framebuffer from guest memory and convert it to a PNG.
Usage: qemu_test.py out/a30s-hello.Image out/shot.png [secs]"""
import os, socket, subprocess, sys, time

img, png = sys.argv[1], sys.argv[2]
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 3
FB, W, H = 0xEC000000, 720, 1560
raw = png + ".raw"
sock = png + ".mon"
if os.path.exists(sock):
    os.unlink(sock)
q = subprocess.Popen(["qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a53", "-m", "4096",
                      "-display", "none", "-serial", "none",
                      "-monitor", f"unix:{sock},server,nowait", "-kernel", img],
                     stderr=subprocess.PIPE)
for _ in range(50):
    if os.path.exists(sock):
        break
    time.sleep(0.1)
s = socket.socket(socket.AF_UNIX)
s.connect(sock)
time.sleep(secs)
s.sendall(f"pmemsave {FB:#x} {W*H*4} {raw}\n".encode())
time.sleep(1.5)
s.sendall(b"quit\n")
try:
    q.wait(5)
except subprocess.TimeoutExpired:
    q.kill()
err = q.stderr.read().decode()
if err.strip():
    print("qemu stderr:", err[:300])
subprocess.check_call(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "bgra",
                       "-s", f"{W}x{H}", "-i", raw, png])
os.unlink(raw)
print("wrote", png)
