"""SWSE Setup - install SWSE into Oddworld: Stranger's Wrath HD, and choose what
it switches on. The players' app: "SWSE Setup.exe" in the release zip.

One screen, the same one the modders' Mod Loader (studio.py) shows as its
first tab: oddforge/setupui.py. Nothing here touches the game's archives, so
it needs no Pillow and builds small.

Run:         python swse_setup.py
Build .exe:  python -m PyInstaller SWSESetup.spec --distpath release
             (release\\SWSE Setup.exe, which tools\\package_swse.ps1 ships)
Check:       "SWSE Setup.exe" --selftest <file>   builds the window unseen,
             writes what it found to <file>, exits 0 when all is well
"""
from __future__ import annotations

import sys
import tkinter as tk

from oddforge import setupui


class SetupApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.configure(bg=setupui.DARK)
        setupui.apply_theme(self)
        self.panel = setupui.SetupPanel(self)
        version = setupui.swse_version(self.panel.source)
        self.title("SWSE Setup" + (f" {version}" if version else ""))
        self.geometry("1060x700")
        self.minsize(900, 560)
        self.panel.pack(fill="both", expand=True, padx=10, pady=(4, 8))


def main(argv: list[str]) -> int:
    if len(argv) >= 3 and argv[1] == "--selftest":
        def build():
            app = SetupApp()
            return app, app.panel, f"window '{app.title()}': the setup screen, no tabs"
        return setupui.run_selftest(argv[2], build)
    SetupApp().mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
