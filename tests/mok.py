#!/usr/bin/env python3
"""Boots a stick image under Secure Boot with Microsoft's keys and enrolls SYNTHOS\\SYNTHOS.CER in MokManager by
pressing the keys a person would; prints the serial output of the boot that follows.
usage: mok.py OVMF_CODE.fd OVMF_VARS.fd disk.img"""
import os, socket, subprocess, sys, tempfile, time

code, vars_fd, disk = sys.argv[1:4]
work = tempfile.mkdtemp()
sock, serial = os.path.join(work, "mon"), os.path.join(work, "serial.txt")
q = subprocess.Popen(["qemu-system-x86_64", "-machine", "q35,smm=on", "-global", "driver=cfi.pflash01,property=secure,value=on",
                      "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code}",
                      "-drive", f"if=pflash,format=raw,unit=1,file={vars_fd}", "-drive", f"format=raw,file={disk}",
                      "-m", "256", "-nic", "none", "-display", "none", "-monitor", f"unix:{sock},server,nowait",
                      "-serial", f"file:{serial}"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(100):
    if os.path.exists(sock):
        break
    time.sleep(0.1)
mon = socket.socket(socket.AF_UNIX)
mon.connect(sock)


def keys(*names, pause=2.0):
    for k in names:
        mon.sendall(f"sendkey {k}\n".encode())
        time.sleep(0.3)
    time.sleep(pause)


def serial_has(text):
    try:
        return text in open(serial, errors="replace").read()
    except FileNotFoundError:
        return False


time.sleep(20)
keys("ret", pause=3)                       # "Verification failed" -> OK
keys("ret")                                # "Press any key to perform MOK management"
keys("down", "ret")                        # Enroll key from disk
keys("ret")                                # the stick's EFI partition
keys("down", "ret")                        # SYNTHOS/
keys("down", "down", "down", "ret")        # SYNTHOS.CER
keys("down", "ret")                        # Continue
keys("down", "ret")                        # Yes
keys("ret", pause=0)                       # Reboot
for _ in range(120):
    if serial_has("Shell ready"):
        break
    time.sleep(0.5)
q.kill()
q.wait()
print(open(serial, errors="replace").read().replace("\r", "") if os.path.exists(serial) else "")
