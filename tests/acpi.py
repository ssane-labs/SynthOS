"""ACPI events in QEMU: a test SSDT (tests/aml.py) adds a lid, a power button that Notifies, and methods that check
what the AML interpreter computes; QMP then hot-plugs a PCI card (a GPE whose _E01 runs in QEMU's own DSDT), wakes the
machine from S3 sleep (the shell, the disk, the e1000 and both processors must come back) and presses the power button.
usage: tests/acpi.py IMAGE"""
import codecs
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aml import *  # noqa: E402,F403


def test_table():
    lid = Device("LID0", Name("_HID", EisaId("PNP0C0D")), Name("LIDS", One),
                 Method("_LID", 0, Return(N("LIDS"))),
                 Method("TOGL", 0, Store(XOr(N("LIDS"), One), N("LIDS")), Notify(N("LID0"), Int(0x80))))
    # a battery at half charge, discharging at 5 W (five hours left), and an unplugged AC adapter; LOWB drops it to 8 %
    battery = Device("BAT0", Name("_HID", EisaId("PNP0C0A")), Method("_STA", 0, Return(Int(0x1F))),
                     Name("BSTV", Package(Int(1), Int(5000), Int(25000), Int(12000))),
                     Method("_BIF", 0, Return(Package(Zero, Int(60000), Int(50000), One, Int(12000), Int(5000),
                                                      Int(2000), One, One, Str("TestCell"), Str("42"), Str("LION"),
                                                      Str("SynthLab")))),
                     Method("_BST", 0, Return(N("BSTV"))),
                     Method("LOWB", 0, Store(Int(4000), Index(N("BSTV"), Int(2))), Notify(N("BAT0"), Int(0x80))))
    adapter = Device("ADP0", Name("_HID", Str("ACPI0003")), Method("_PSR", 0, Return(Zero)))
    # an I2C HID touchpad at 0x2c on a controller that is not there (QEMU has no DesignWare I2C): the enumeration only
    source = b"\\_SB.I2C1\x00"
    i2c = bytes([1, 0, 1, 2, 0, 0, 1, 6, 0]) + (400000).to_bytes(4, "little") + (0x2c).to_bytes(2, "little") + source
    touchpad = Device("TPD0", Name("_HID", Str("SYNA0001")), Name("_CID", EisaId("PNP0C50")),
                      Name("_CRS", Buffer(b"\x8e" + len(i2c).to_bytes(2, "little") + i2c + b"\x79\x00")),
                      Method("_DSM", 4, If(LEqual(Arg(2), One), Return(Int(0x20))), Return(Buffer([0]))))
    controller = Device("I2C1", Name("_ADR", Int(0x00150001)))
    # an ACPI video panel (QEMU's display is not Intel's): _BCM takes the level nearest the brightness asked for
    video = Device("GFX0", Name("_ADR", Int(0x00020000)), Method("_DOS", 1, Store(Arg(0), N("\\DOSV"))),
                   Device("LCD0", Name("_ADR", Int(0x400)),
                          Method("_BCL", 0, Return(Package(*[Int(v) for v in (100, 50, 10, 25, 50, 75, 100)]))),
                          Method("_BCM", 1, Store(Arg(0), N("\\BCMV"))),
                          Method("UPKY", 0, Notify(N("LCD0"), Int(0x86)))))
    button = Device("PBTN", Name("_HID", EisaId("PNP0C0C")), Method("PRES", 0, Notify(N("PBTN"), Int(0x80))))
    checks = Method(
        "\\TST1", 0,
        Store(Int(5), Local(0)),
        check(1, Add(Local(0), Int(3)), Int(8)),
        Store(Zero, Local(1)), Store(Zero, Local(2)),
        While(LLess(Local(1), Int(10)), Increment(Local(1)), Store(Add(Local(2), Local(1)), Local(2))),
        check(2, Local(2), Int(55)),
        CreateDWordField(N("BUF0"), Int(4), "DW01"),
        check(3, N("DW01"), Int(0x08070605)),
        Store(Int(0x11223344), N("DW01")),
        check(4, DerefOf(Index(N("BUF0"), Int(4))), Int(0x44)),
        check(5, DerefOf(Index(N("PKG0"), Zero)), Int(10)),
        check(6, SizeOf(N("PKG0")), Int(4)),
        check(7, DerefOf(Index(DerefOf(Index(N("PKG0"), Int(2))), Zero)), Int(7)),
        check(8, Call("\\ADD2", Int(40), Int(2)), Int(42)),
        check(9, Concat(Str("ab"), Str("cd")), Str("abcd")),
        check(10, ObjectType(N("PKG0")), Int(4)),
        check(11, Divide(Int(17), Int(5), Local(3)), Int(3)),
        check(12, Local(3), Int(2)),
        check(13, LNotEqual(N("EBDA"), Zero), Ones),
        Store(Int(0x5a), N("CMSE")),
        check(14, N("CMSE"), Int(0x5a)),
        check(15, Call("\\_OSI", Str("Windows 2015")), Ones),
        check(16, CondRefOf(N("\\NOPE")), Zero),
        check(17, Match(N("PKG1"), 1, Int(30), 0, Zero, Zero), Int(2)),
        check(18, ToInteger(Str("0x1F")), Int(31)),
        check(19, Mid(Str("hello"), One, Int(3)), Str("ell")),
        Store(Int(9), Index(N("PKG1"), One)),
        check(20, DerefOf(Index(N("PKG1"), One)), Int(9)),
        If(LEqual(One, Zero), Return(Int(21)), ), Else(Store(Int(77), Local(4))),
        check(22, Local(4), Int(77)),
        check(23, ShiftLeft(One, Int(20)), Int(1 << 20)),  # QEMU's DSDT is revision 1: 32-bit integers
        check(24, Add(Ones, One), Zero),
        Return(Zero))
    return ssdt(
        Scope("\\_SB", lid, button, battery, adapter, touchpad, controller, video),
        Name("\\BCMV", Zero),
        Name("\\DOSV", Zero),
        Name("\\BUF0", Buffer([1, 2, 3, 4, 5, 6, 7, 8])),
        Name("\\PKG0", Package(Int(10), Str("ab"), Package(Int(7)), N("\\_SB.LID0"))),
        Name("\\PKG1", Package(Int(10), Int(20), Int(30))),
        OperationRegion("\\BDA0", 0, 0x400, 0x100),
        Field("\\BDA0", 0x02, (None, 0x0E * 8), ("EBDA", 16)),
        OperationRegion("\\CMS0", 1, 0x70, 2),
        Field("\\CMS0", 0x01, ("CIDX", 8), ("CDAT", 8)),
        IndexField("CIDX", "CDAT", 0x01, (None, 0x0E * 8), ("CMSE", 8)),
        Method("\\ADD2", 2, Return(Add(Arg(0), Arg(1)))),
        Method("\\RECU", 0, Return(Call("\\RECU"))),
        Method("\\LOOP", 0, While(One, b"\xa3")),
        checks)


class Machine:
    def __init__(self, image, work):
        shutil.copy(image, os.path.join(work, "disk.img"))
        with open(os.path.join(work, "test.aml"), "wb") as f:
            f.write(test_table())
        self.qmp_path = os.path.join(work, "qmp")
        self.out = ""
        self.lock = threading.Lock()
        self.proc = subprocess.Popen(
            [os.environ.get("QEMU", "qemu-system-x86_64"),
             "-drive", "format=raw,file=" + os.path.join(work, "disk.img"),
             "-m", "64", "-smp", "2", "-display", "none", "-serial", "stdio", "-nic", "user,model=e1000",
             "-device", "virtio-multitouch-pci",
             "-acpitable", "file=" + os.path.join(work, "test.aml"),
             "-qmp", "unix:%s,server=on,wait=off" % self.qmp_path],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        decoder = codecs.getincrementaldecoder("utf-8")("replace")
        while True:
            b = self.proc.stdout.read1(4096)
            if not b:
                return
            with self.lock:
                self.out += decoder.decode(b).replace("\r", "")

    def wait_for(self, text, start, timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                if text in self.out[start:]:
                    return True
            if self.proc.poll() is not None:
                time.sleep(0.5)
                with self.lock:
                    return text in self.out[start:]
            time.sleep(0.1)
        return False

    def run(self, command, timeout=60):
        with self.lock:
            start = len(self.out)
        self.proc.stdin.write((command + "\n").encode())
        self.proc.stdin.flush()
        self.wait_for("►", start + len(command), timeout)
        time.sleep(0.2)
        with self.lock:
            out = self.out[start:]
        if os.environ.get("ACPI_VERBOSE"):
            print(out)
        return out

    def qmp(self, *commands):
        s = socket.socket(socket.AF_UNIX)
        s.connect(self.qmp_path)
        f = s.makefile("rw")
        f.readline()
        replies = []
        for c in ({"execute": "qmp_capabilities"},) + commands:
            f.write(json.dumps(c) + "\n")
            f.flush()
            while True:
                reply = json.loads(f.readline())
                if "return" in reply or "error" in reply:
                    replies.append(reply)
                    break
        s.close()
        return replies[1:]

    def touch(self, *frames):
        """Sends frames of fingers (lists of (x, y), 0..0x7fff) to the touch screen, each one input event; an empty
        frame lifts every finger of the frame before it."""
        commands, last = [], []
        for fingers in frames:
            events = []
            for slot in range(max(len(fingers), len(last))):
                if slot >= len(fingers):
                    events.append(mtt("end", slot, -1))
                    continue
                events.append(mtt("begin" if slot >= len(last) else "update", slot, slot + 1))
                events.append(mtt("data", slot, slot + 1, "x", fingers[slot][0]))
                events.append(mtt("data", slot, slot + 1, "y", fingers[slot][1]))
            commands.append({"execute": "input-send-event", "arguments": {"events": events}})
            last = fingers
        return self.qmp(*commands)


def mtt(kind, slot, tracking, axis="x", value=0):
    return {"type": "mtt", "data": {"type": kind, "slot": slot, "tracking-id": tracking, "axis": axis, "value": value}}


def main():
    image = sys.argv[1]
    work = tempfile.mkdtemp()
    passed = failed = 0

    def expect(what, text, output):
        nonlocal passed, failed
        if text in output:
            passed += 1
        else:
            failed += 1
            print("  FAIL  %s\n        expected: %s\n        got: %s" % (what, text, output.strip()[-400:]))

    m = Machine(image, work)
    try:
        if not m.wait_for("►", 0, 120):
            print("acpi: the system did not boot")
            print(m.out[-2000:])
            sys.exit(1)
        expect("boot", "Power: ACPI, SCI on IRQ 9", m.out)
        expect("boot", ", lid", m.out)
        expect("acpi", "lid: open", m.run("acpi"))
        expect("acpi devices", "\\_SB_.LID0", m.run("acpi devices"))
        expect("AML checks", "TST1 = 0x0", m.run("acpi eval TST1"))
        expect("recursion stops", "methods nest too deep", m.run("acpi eval RECU"))
        expect("endless loop stops", "While runs too long", m.run("acpi eval LOOP", 120))
        out = m.run("battery")
        expect("battery", "BAT0 (TestCell): 50%, discharging, 5 h 00 min left", out)
        expect("battery health", "25.0 Wh of 50.0 Wh (new: 60.0 Wh, health 83%), 5.0 W", out)
        expect("AC adapter", "AC adapter: not plugged in", out)
        with m.lock:
            start = len(m.out)
        m.run("acpi eval _SB.BAT0.LOWB")
        expect("low battery warning", "Battery: 8% of the battery is left (about 48 min)",
               m.out[start:] if m.wait_for("Battery: 8%", start, 20) else m.out[start:])
        # 64-bit only: a tap, a two-finger tap, scrolling and a three-finger swipe up on the touch screen
        if "virtio-input:" in m.run("dmesg | grep virtio-input"):
            m.touch([(8000, 8000)], [])
            time.sleep(0.4)
            m.touch([(8000, 8000), (12000, 8000)], [])
            time.sleep(0.4)
            pts = [(10000, 10000), (14000, 10000)]
            m.touch(*[[(x, y + i * 1000) for x, y in pts] for i in range(11)], [])
            time.sleep(0.4)
            pts = [(12000, 24000), (16000, 25000), (20000, 24000)]
            m.touch(*[[(x, y - i * 1200) for x, y in pts] for i in range(11)], [])
            time.sleep(0.4)
            out = m.run("touchpad")
            expect("touch screen", "QEMU Virtio MultiTouch (touch screen)", out)
            expect("touch gestures", "gestures: taps 2, scroll steps 7, swipes 1", out)
            expect("three-finger swipe", "touch: three-finger swipe up", m.run("dmesg | grep swipe"))
            out = m.run("dmesg | grep i2c-hid")
            expect("I2C HID touchpad",
                   "i2c-hid: \\_SB_.TPD0 at 0x2c on \\_SB.I2C1 (400 kHz), descriptor at 0x0020", out)
            expect("I2C controller", "i2c-hid: \\_SB.I2C1: no controller on PCI", out)
        expect("ACPI backlight", "acpi: backlight \\_SB_.GFX0.LCD0, 5 levels", m.run("dmesg | grep backlight"))
        expect("_DOS: the system sets the level", "0x4", m.run("acpi eval DOSV"))
        m.run("config brightness 40")
        expect("_BCM nearest level", "0x32", m.run("acpi eval BCMV"))
        m.run("config brightness 20")
        expect("_BCM lower level", "0x19", m.run("acpi eval BCMV"))
        m.run("acpi eval _SB.GFX0.LCD0.UPKY")
        expect("brightness key", "acpi: brightness up", m.run("dmesg | grep brightness"))
        m.run("acpi eval _SB.LID0.TOGL")
        expect("lid closed", "lid: closed", m.run("acpi"))
        expect("lid in the log", "acpi: lid closed", m.run("dmesg | grep lid"))
        # a hot-plugged card: GPE 1, whose _E01 Notifies the slot
        m.qmp({"execute": "device_add", "arguments": {"driver": "e1000", "id": "hot1"}})
        time.sleep(2)
        expect("GPE method ran", "acpi: notify \\_SB_.PCI0.S", m.run("dmesg | grep notify"))
        out = m.run("acpi")
        expect("SCI interrupts", "SCI: IRQ 9", out)
        if ", 0 interrupts" in out:
            expect("SCI interrupts arrive", "interrupts (more than 0)", out)
        # the power button as a device that Notifies, with the button set to do nothing
        m.run("config powerbutton nothing")
        m.run("acpi eval _SB.PBTN.PRES")
        expect("control-method power button", "acpi: power button", m.run("dmesg | grep button"))
        m.run("config powerbutton ask")
        # sleep in S3: QEMU stops the machine until QMP wakes it; then the shell, the disk and the clock still work
        expect("S3 offered", "S3 sleep is available", m.run("suspend -n"))
        with m.lock:
            start = len(m.out)
        m.proc.stdin.write(b"suspend\n")
        m.proc.stdin.flush()
        m.wait_for("Sleeping (S3)", start, 20)
        time.sleep(2)
        status = m.qmp({"execute": "query-status"})
        expect("asleep", "'suspended'", str(status))
        m.qmp({"execute": "system_wakeup"})
        expect("awake", "Awake.", m.out[start:] if m.wait_for("Awake.", start, 30) else m.out[start:])
        m.wait_for("►", start, 10)
        expect("disk after sleep", "after-s3", m.run("echo after-s3 > /home/s3.txt; cat /home/s3.txt"))
        expect("network after sleep", "2 received", m.run("ping -c 2 10.0.2.2", 30))
        out = m.run("dmesg | grep sleep")
        expect("processors after sleep", "2 processors after sleep", out)
        expect("time after sleep", "sleep: awake after", out)
        # the fixed power button: the shell shuts down
        with m.lock:
            start = len(m.out)
        m.qmp({"execute": "system_powerdown"})
        expect("power button shuts down", "Power button: shutting down",
               m.out[start:] if m.wait_for("Power button: shutting down", start, 30) else m.out[start:])
        try:
            m.proc.wait(30)
            passed += 1
        except subprocess.TimeoutExpired:
            failed += 1
            print("  FAIL  QEMU did not power off after the power button")
    finally:
        if m.proc.poll() is None:
            m.proc.kill()
        shutil.rmtree(work, ignore_errors=True)
    print("acpi tests: %d passed, %d failed" % (passed, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
