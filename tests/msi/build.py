#!/usr/bin/env python3
"""Builds the custom-action test packages: wixl writes the package from the .wxs, then msibuild adds the
CustomAction rows and their places in the InstallExecuteSequence (wixl cannot write most kinds of custom action).
usage: tests/msi/build.py <output dir>   (needs wixl and msibuild from msitools)"""
import os, subprocess, sys, tempfile

DEFERRED, ROLLBACK, IGNORE = 0x400, 0x100, 0x40
PACKAGES = {
    'synthca': [  # name, type, source, target, condition, sequence
        ('SetGreeting', 51, 'GREETING', 'hi [ProductName]', '', 990),
        ('MoveDir', 35, 'INSTALLDIR', '[ProgramFilesFolder]SynthCA Moved', 'NOT Installed AND (VersionNT > 500 OR Privileged)', 995),
        ('TooOld', 19, '', 'This must not show.', 'VersionNT < 500', 996),
        ('RunFromDir', 34 | DEFERRED | IGNORE, 'INSTALLDIR', '"C:\\apps\\wintest.exe" opened [GREETING]', 'GREETING >< "Synth"', 4001),
        ('RunFromProp', 50 | DEFERRED, 'WINTEST', 'info', '', 4002),
        ('Script', 38, '', 'MsgBox "hi"', '', 4003),
    ],
    'synthfail': [
        ('Undo', 34 | DEFERRED | ROLLBACK | IGNORE, 'INSTALLDIR', '"C:\\apps\\wintest.exe" opened undo', '', 4001),
        ('Fails', 34 | DEFERRED, 'INSTALLDIR', '"C:\\apps\\wintest.exe" opened broken', '', 4002),
    ],
}

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.abspath(sys.argv[1])
for name, actions in PACKAGES.items():
    msi = os.path.join(out, name + '.msi')
    subprocess.run(['wixl', '-o', msi, name + '.wxs'], cwd=here, check=True)
    with tempfile.TemporaryDirectory() as tmp:
        seq = subprocess.run(['msiinfo', 'export', msi, 'InstallExecuteSequence'], capture_output=True, text=True,
                             check=True).stdout.rstrip('\n').split('\n')
        seq += ['%s\t%s\t%d' % (a, cond, n) for a, _, _, _, cond, n in actions]
        with open(os.path.join(tmp, 'InstallExecuteSequence.idt'), 'w') as f:
            f.write('\r\n'.join(seq) + '\r\n')
        rows = ['Action\tType\tSource\tTarget', 's72\ti2\tS72\tS255', 'CustomAction\tAction']
        rows += ['%s\t%d\t%s\t%s' % (a, t, s, tg) for a, t, s, tg, _, _ in actions]
        with open(os.path.join(tmp, 'CustomAction.idt'), 'w') as f:
            f.write('\r\n'.join(rows) + '\r\n')
        for table in ('InstallExecuteSequence', 'CustomAction'):
            subprocess.run(['msibuild', msi, '-i', os.path.join(tmp, table + '.idt')], check=True)
