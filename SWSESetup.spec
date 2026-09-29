# -*- mode: python ; coding: utf-8 -*-
# "SWSE Setup.exe" - the players' installer: swse_setup.py, one screen
# (oddforge/setupui.py). Build from the repository root:
#
#     python -m PyInstaller SWSESetup.spec --distpath release
#
# -> release\SWSE Setup.exe (git-ignored), which tools\package_swse.ps1 ships.
# It must sit beside bin\ and SWSEMods\ (the unzipped release) to install.
# Pillow and the archive tools are the Mod Loader's (ModLoader.spec); nothing
# here imports them, and they are excluded so the exe stays small.


a = Analysis(
    ['swse_setup.py'],
    pathex=[],
    binaries=[],
    datas=[],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=['PIL', 'numpy'],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='SWSE Setup',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
