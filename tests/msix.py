#!/usr/bin/env python3
"""Makes the MSIX test packages: a desktop program (with a percent-encoded part name), a bundle with one package
per processor, and a Universal Windows app. usage: msix.py <dir>"""
import io, sys, zipfile

def manifest(arch, uwp=False):
    app = ('<Application Id="App" Executable="UWPApp.exe" EntryPoint="UWPApp.App">'
           '<uap:VisualElements DisplayName="UWP Sample" /></Application>' if uwp else
           '<Application Id="Tool" Executable="bin\\My Tool.exe" EntryPoint="Windows.FullTrustApplication">'
           '<uap:VisualElements DisplayName="Sample Tool" Square150x150Logo="Assets\\Logo.png" /></Application>')
    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10" '
            'xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10">'
            f'<Identity Name="Synth.SampleTool" Version="1.2.3.0" Publisher="CN=Sample &amp; Co" '
            f'ProcessorArchitecture="{arch}" />'
            '<Properties><DisplayName>Sample Tool</DisplayName><PublisherDisplayName>Sample &amp; Co'
            '</PublisherDisplayName><Logo>Assets\\Logo.png</Logo></Properties>'
            f'<Applications>{app}</Applications></Package>')

def package(arch, uwp=False):
    b = io.BytesIO()
    with zipfile.ZipFile(b, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('AppxManifest.xml', manifest(arch, uwp))
        z.writestr('[Content_Types].xml', '<Types/>')
        z.writestr('bin/My%20Tool.exe', b'MZ' + bytes(200))
        z.writestr('Assets/Logo.scale-100.png', b'')
        z.writestr('AppxBlockMap.xml', '<BlockMap/>')
    return b.getvalue()

d = sys.argv[1]
open(f'{d}/tool.msix', 'wb').write(package('x86'))
open(f'{d}/uwp.msix', 'wb').write(package('neutral', uwp=True))
with zipfile.ZipFile(f'{d}/tool.msixbundle', 'w') as z:
    z.writestr('AppxMetadata/AppxBundleManifest.xml',
               '<Bundle><Packages>'
               '<Package Type="application" Architecture="arm64" FileName="Tool_arm64.msix" />'
               '<Package Type="application" Architecture="x86" FileName="Tool_x86.msix" />'
               '<Package Type="application" Architecture="x64" FileName="Tool_x64.msix" />'
               '</Packages></Bundle>')
    for a in ('arm64', 'x86', 'x64'):
        z.writestr(f'Tool_{a}.msix', package(a))
