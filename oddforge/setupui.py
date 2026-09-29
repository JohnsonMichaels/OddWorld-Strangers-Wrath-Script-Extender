"""The SWSE Setup screen: install SWSE into the game, and choose what it
switches on.

One tkinter frame, SetupPanel, used twice: as the whole window of the
players' app (swse_setup.py, built as "SWSE Setup.exe" by SWSESetup.spec) and
as the first tab of the modders' Mod Loader (studio.py). The logic under it -
features.txt, installing, finding the game - is swsefeatures, swseinstall and
gamepaths, tested without a window by tools/test_swsefeatures.py.
"""
from __future__ import annotations

import re
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import Callable

from . import swsefeatures as sf
from . import swseinstall as si
from .gamepaths import STEAM_DEFAULT_ROOT, GameInstall, check_game_dir, find_game

# The Mod Loader's colours, shared by both windows.
GOLD = "#d6a854"
DARK = "#1c1a17"
PANEL = "#26231f"
TEXT = "#e8e2d4"
DIM = "#8d876f"


def apply_theme(root: tk.Misc) -> ttk.Style:
    """The ttk styles both windows use (the Mod Loader's lists and tabs too)."""
    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure("Treeview", background=PANEL, fieldbackground=PANEL,
                    foreground=TEXT, rowheight=22)
    style.configure("Treeview.Heading", background=DARK, foreground=GOLD)
    style.map("Treeview", background=[("selected", GOLD)],
              foreground=[("selected", DARK)])
    style.configure("TNotebook", background=DARK, borderwidth=0)
    style.configure("TNotebook.Tab", background=PANEL, foreground=TEXT, padding=(14, 6))
    style.map("TNotebook.Tab", background=[("selected", GOLD)],
              foreground=[("selected", DARK)])
    return style


def swse_version(source: Path | None) -> str:
    """The SWSE version the release beside the app installs: its DLL's
    version resource; from a checkout, swse/swse_version.h. "" if neither."""
    if source:
        v = (sf.version_info(source / "bin" / "dinput8.dll") or {}).get("ProductVersion", "")
        if v:
            return v
    if not getattr(sys, "frozen", False):
        h = Path(__file__).resolve().parent.parent / "swse" / "swse_version.h"
        try:
            m = re.search(r'#define\s+SWSE_VERSION\s+"([^"]+)"', h.read_text(encoding="latin-1"))
            if m:
                return m.group(1)
        except OSError:
            pass
    return ""


PRESETS = (
    ("Recommended", "default",
     "The 1.1 default: the console on, AI and player tuning on auto (they run "
     "only while their own file asks for something), everything else off - "
     "plugins too. The game plays as shipped, plus the console."),
    ("Classic SWSE", "full",
     "The classic SWSE, the 1.0.x look: the console, graphics, HD textures, hit "
     "reactions, foliage and AI tuning on; the other switches as they are. The "
     "same as `features preset full` in the console."),
    ("Everything", "everything",
     "Every built-in switch on - the tuning systems and the experimental ray "
     "tracing too. Plugins stay as they are: a plugin only runs when you tick it."),
)


class SetupPanel(tk.Frame):
    """The whole Setup screen. `game`: the install to show (found if None);
    `on_game_changed(game)`: called after Browse picks another folder."""

    def __init__(self, master: tk.Misc, game: GameInstall | None = None, *,
                 on_game_changed: Callable[[GameInstall], None] | None = None) -> None:
        super().__init__(master, bg=DARK)
        self.game = game if game is not None else find_game()
        self.source = si.default_source()
        self.on_game_changed = on_game_changed
        self.vars: dict[str, tk.Variable] = {}
        self.plugins: list[sf.Plugin] = []
        self.expl: dict[str, str] = {}
        self.hd_lbl: tk.Label | None = None
        self.running = False
        bold = ("Segoe UI", 10, "bold")

        where = tk.Frame(self, bg=DARK)
        where.pack(fill="x", padx=4, pady=(6, 2))
        tk.Label(where, text="Game folder", bg=DARK, fg=GOLD, font=bold).grid(row=0, column=0, sticky="w")
        self.game_lbl = tk.Label(where, bg=DARK, fg=TEXT, font=("Segoe UI", 10), anchor="w")
        self.game_lbl.grid(row=0, column=1, sticky="w", padx=8)
        tk.Button(where, text="Browse…", command=self.browse, bg=PANEL, fg=TEXT,
                  relief="flat", padx=10).grid(row=0, column=2, sticky="e")
        tk.Label(where, text="SWSE", bg=DARK, fg=GOLD, font=bold).grid(row=1, column=0, sticky="w")
        self.inst_lbl = tk.Label(where, bg=DARK, fg=TEXT, font=("Segoe UI", 10), anchor="w")
        self.inst_lbl.grid(row=1, column=1, columnspan=2, sticky="w", padx=8)
        where.columnconfigure(1, weight=1)

        body = tk.Frame(self, bg=DARK)
        body.pack(fill="both", expand=True, padx=4, pady=4)
        left = tk.Frame(body, bg=PANEL)
        left.pack(side="left", fill="both", expand=True)
        self.canvas = tk.Canvas(left, bg=PANEL, highlightthickness=0)
        sb = ttk.Scrollbar(left, orient="vertical", command=self.canvas.yview)
        sb.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True, padx=(8, 0), pady=6)
        self.canvas.configure(yscrollcommand=sb.set)
        self.rows = tk.Frame(self.canvas, bg=PANEL)
        self.canvas.create_window((0, 0), window=self.rows, anchor="nw")
        self.rows.bind("<Configure>", lambda e: self.canvas.configure(
            scrollregion=self.canvas.bbox("all")))

        # The wheel scrolls the list while the pointer is anywhere over it (its
        # rows are child windows, so a binding on the canvas alone misses them).
        def wheel(e) -> None:
            w = self.winfo_containing(e.x_root, e.y_root)
            if w is not None and str(w).startswith(str(self.canvas)):
                self.canvas.yview_scroll(-1 if e.delta > 0 else 1, "units")
        self.bind_all("<MouseWheel>", wheel, add="+")

        right = tk.Frame(body, bg=PANEL, width=330)
        right.pack(side="right", fill="y", padx=(6, 0))
        right.pack_propagate(False)
        tk.Label(right, text="About", bg=PANEL, fg=GOLD, font=bold).pack(anchor="w", padx=10, pady=(8, 2))
        self.about = tk.Label(right, bg=PANEL, fg=TEXT, justify="left", anchor="nw",
                              wraplength=305, font=("Segoe UI", 9))
        self.about.pack(fill="both", expand=True, padx=10)

        bar = tk.Frame(self, bg=DARK)
        bar.pack(fill="x", padx=4, pady=2)
        tk.Label(bar, text="Presets", bg=DARK, fg=GOLD, font=bold).pack(side="left", padx=(0, 6))
        for label, preset, tip in PRESETS:
            b = tk.Button(bar, text=label, command=lambda p=preset: self.preset(p),
                          bg=PANEL, fg=GOLD, relief="flat", padx=10)
            b.pack(side="left", padx=3)
            b.bind("<Enter>", lambda e, t=tip: self.about.config(text=t))
        tk.Button(bar, text="Uninstall", command=self.uninstall, bg=PANEL, fg=TEXT,
                  relief="flat", padx=10).pack(side="right", padx=3)
        tk.Button(bar, text="Apply switches", command=self.apply, bg=PANEL, fg=GOLD,
                  relief="flat", font=bold, padx=12).pack(side="right", padx=3)
        tk.Button(bar, text="Install / Update", command=self.install, bg=GOLD, fg=DARK,
                  relief="flat", font=("Segoe UI", 11, "bold"), padx=16).pack(side="right", padx=3)

        self.note = tk.Label(self, bg=DARK, fg=DIM, font=("Segoe UI", 9), anchor="w",
                             justify="left", wraplength=900)
        self.note.pack(fill="x", padx=6)
        self.note.bind("<Configure>", lambda e: self.note.config(wraplength=max(200, e.width - 12)))
        self.log = tk.Text(self, bg=PANEL, fg=TEXT, relief="flat", font=("Consolas", 9),
                           height=6, wrap="word")
        self.log.pack(fill="x", padx=4, pady=(2, 6))
        self.load()
        self.after(3000, self._poll)

    # ---------------------------------------------------------------- display
    def say(self, msg: str) -> None:
        self.log.insert("end", msg + "\n")
        self.log.see("end")

    def _note_text(self) -> str:
        if self.running:
            return ("The game is running: Apply switches them now as well, through the console "
                    "mailbox - the same as typing `features <name> on` in the console (~). "
                    "Install and Uninstall wait until you close the game.")
        return ("Switches take effect when the game starts. When it is running, Apply also switches "
                "them live through the console mailbox - or type `features <name> on` in the console (~).")

    def _poll(self) -> None:
        g = self.game
        running = bool(g) and sf.game_running(g.exe)
        if running != self.running:
            self.running = running
            self.note.config(text=self._note_text())
        self.after(3000, self._poll)

    def load(self) -> None:
        """(Re)read the game folder: what is installed, the switches, the plugins."""
        for w in self.rows.winfo_children():
            w.destroy()
        self.vars.clear()
        self.hd_lbl = None
        g, src = self.game, self.source
        src_ver = swse_version(src) if src else ""
        src_txt = (f"Install / Update uses {src}" + (f"  (SWSE {src_ver})" if src_ver else "")) if src \
            else "no SWSE release beside this program - run it from the unzipped download"
        text = None
        if g:
            self.game_lbl.config(text=f"{g.root}    ({g.store})")
            installed, _ = si.installed_version(g)
            self.inst_lbl.config(text=f"in this folder: {installed}      ·      {src_txt}")
            text = sf.read_text(g.mods / "features.txt")
        else:
            self.game_lbl.config(text="not found - Browse to the Stranger's Wrath folder "
                                      "(the one holding bin\\ and data\\)")
            self.inst_lbl.config(text=src_txt)
        ff = sf.parse(text)
        self.expl = sf.explanations(text)
        if not self.expl and src:
            self.expl = sf.explanations(sf.read_text(src / "SWSEMods" / "features.txt"))
        notes: list[str] = []
        self.plugins = []
        if g:
            self.plugins, notes = sf.discover_plugins(g.mods)
        self.running = bool(g) and sf.game_running(g.exe)
        self.note.config(text=self._note_text())

        rows = self.rows
        mono = ("Consolas", 10, "bold")

        def about(widgets, msg: str) -> None:
            for w in widgets:
                w.bind("<Enter>", lambda e, m=msg: self.about.config(text=m))

        def box(parent, var) -> tk.Checkbutton:
            return tk.Checkbutton(parent, variable=var, bg=PANEL, activebackground=PANEL,
                                  selectcolor=DARK, fg=TEXT, activeforeground=TEXT,
                                  highlightthickness=0, bd=0)

        r = 0
        tk.Label(rows, text="SWSE's systems - tick what you want on", bg=PANEL, fg=GOLD,
                 font=("Segoe UI", 10, "bold")).grid(row=r, column=0, columnspan=4, sticky="w", pady=(0, 4))
        r += 1
        for s in sf.BUILTINS:
            state = ff.states[s.name]
            if s.auto_capable:
                var: tk.Variable = tk.StringVar(value=state)
                ctl = tk.Frame(rows, bg=PANEL)
                for val in ("auto", "on", "off"):
                    tk.Radiobutton(ctl, text=val, value=val, variable=var, bg=PANEL, fg=TEXT,
                                   activebackground=PANEL, activeforeground=TEXT, selectcolor=DARK,
                                   highlightthickness=0, bd=0, font=("Segoe UI", 9)).pack(side="left")
            else:
                var = tk.BooleanVar(value=state == "on")
                ctl = box(rows, var)
            self.vars[s.name] = var
            ctl.grid(row=r, column=0, sticky="w", padx=(0, 6), pady=1)
            name = tk.Label(rows, text=s.name, bg=PANEL, fg=GOLD, font=mono, width=11, anchor="w")
            name.grid(row=r, column=1, sticky="w")
            desc = tk.Label(rows, text=s.oneliner, bg=PANEL, fg=TEXT, font=("Segoe UI", 10), anchor="w")
            desc.grid(row=r, column=2, sticky="w", padx=(4, 8))
            widgets = [ctl, name, desc]
            if s.name == "hdtextures":
                r += 1
                self.hd_lbl = tk.Label(rows, bg=PANEL, fg=DIM, font=("Segoe UI", 9), anchor="w",
                                       justify="left", wraplength=440)
                self.hd_lbl.grid(row=r, column=2, sticky="w", padx=(4, 8))
                var.trace_add("write", lambda *a: self.hd_status(offer=True))
                widgets.append(self.hd_lbl)
            expl = self.expl.get(s.name) or s.oneliner
            if s.auto_capable:
                expl += "\n\nauto: it runs only while its own file asks for something."
            if s.name in sf.NEXT_LEVEL:
                expl += "\n\nSwitched on in the middle of a game, it takes full effect from the next level load."
            about(widgets, f"{s.name}\n\n{expl}")
            r += 1
        if self.plugins or notes:
            tk.Label(rows, text="Plugins - a mod's own code, off until you tick it", bg=PANEL,
                     fg=GOLD, font=("Segoe UI", 10, "bold")).grid(row=r, column=0, columnspan=4,
                                                                sticky="w", pady=(10, 4))
            r += 1
        for p in self.plugins:
            var = tk.BooleanVar(value=ff.plugin_state(p.name) == "on")
            self.vars[p.name] = var
            ctl = box(rows, var)
            ctl.grid(row=r, column=0, sticky="w", padx=(0, 6), pady=1)
            name = tk.Label(rows, text=p.name, bg=PANEL, fg=GOLD, font=mono, width=11, anchor="w")
            name.grid(row=r, column=1, sticky="w")
            desc = tk.Label(rows, text=f"{p.description or 'a plugin'}  (from {p.mod})",
                            bg=PANEL, fg=TEXT, font=("Segoe UI", 10), anchor="w", justify="left",
                            wraplength=440)
            desc.grid(row=r, column=2, sticky="w", padx=(4, 8))
            about([ctl, name, desc],
                  f"{p.name} - a plugin from the mod '{p.mod}'"
                  + (f", version {p.version}" if p.version else "") + ".\n\n"
                  + (p.description + "\n\n" if p.description else "")
                  + "A plugin is native code with the game's full rights: tick it only for a mod "
                    "from someone you trust. SWSE does not even load the DLL while it is off.")
            r += 1
        for n in notes:
            tk.Label(rows, text=n, bg=PANEL, fg=DIM, font=("Segoe UI", 8), anchor="w", justify="left",
                     wraplength=600).grid(row=r, column=0, columnspan=3, sticky="w")
            r += 1
        self.hd_status()
        self.about.config(text="Point at a switch to read what it does.\n\n"
                               "Install / Update copies SWSE into the game folder and writes "
                               "these switches. Apply writes only the switches.")

    def states(self) -> dict[str, str]:
        out: dict[str, str] = {}
        for name, var in self.vars.items():
            v = var.get()
            out[name] = v if isinstance(v, str) else ("on" if v else "off")
        return out

    def hd_status(self, offer: bool = False) -> None:
        """The HD pack's state, under its switch. `offer`: the user just ticked
        it, so offer the 4GB patcher if the exe still needs it."""
        lbl, g = self.hd_lbl, self.game
        if not lbl:
            return
        on = bool(self.vars.get("hdtextures") and self.vars["hdtextures"].get())
        if not on or not g:
            lbl.config(text="")
            lbl.grid_remove()               # no empty line under the switch
            return
        n = si.hd_textures(g)
        laa = si.large_address_aware(g.exe)
        bits = [f"{n} HD textures found" if n else
                "no HD textures in SWSEMods\\SWSE HD\\textures - the pack is a separate download"]
        if laa is False:
            bits.append("4GB patch NOT applied - the HD pack needs it")
        elif laa:
            bits.append("4GB patch applied")
        lbl.config(text="  ·  ".join(bits))
        lbl.grid()
        if offer and laa is False and self._patcher():
            self.after(50, self._offer_patch)

    def _patcher(self) -> Path | None:
        for d in (self.source, Path(sys.executable).resolve().parent
                  if getattr(sys, "frozen", False) else None):
            if d and (d / "SWSE_4GB_Patcher.exe").is_file():
                return d / "SWSE_4GB_Patcher.exe"
        return None

    def _offer_patch(self) -> None:
        g, patcher = self.game, self._patcher()
        if not g or not patcher:
            return
        if not messagebox.askyesno(
                "SWSE Setup - 4GB patch",
                "HD textures need the 4GB patch: the game is 32-bit, and without it the HD "
                "pack runs out of memory on level load.\n\n"
                f"Run {patcher.name} on\n{g.exe}\nnow? It backs the exe up first, and "
                "--restore puts the original back. (Steam's Verify undoes it too.)"):
            return
        if sf.game_running(g.exe):
            messagebox.showinfo("SWSE Setup", "Close the game first.")
            return
        subprocess.Popen([str(patcher), "--exe", str(g.exe)],
                         creationflags=getattr(subprocess, "CREATE_NEW_CONSOLE", 0))
        self.say(f"started {patcher.name} in its own window - if the game is under "
                 "Program Files and it cannot write, run it as administrator")

    # ---------------------------------------------------------------- actions
    def preset(self, preset: str) -> None:
        states = sf.apply_preset(self.states(), preset, [p.name for p in self.plugins])
        for name, value in states.items():
            var = self.vars.get(name)
            if var is None:
                continue
            var.set(value if isinstance(var, tk.StringVar) else value == "on")
        self.say(f"preset: {dict(default='Recommended', full='Classic SWSE', everything='Everything')[preset]}"
                 " - ticked; Install / Update or Apply switches to use it")

    def browse(self) -> None:
        start = self.game.root if self.game else STEAM_DEFAULT_ROOT.parent
        d = filedialog.askdirectory(title="Your Stranger's Wrath folder (it holds bin and data)",
                                    initialdir=str(start) if Path(start).is_dir() else None)
        if not d:
            return
        g = check_game_dir(d, "chosen")
        if not g:
            messagebox.showerror("SWSE Setup", f"No stranger.exe in\n{d}\nor in its bin folder.")
            return
        self.game = g
        if self.on_game_changed:
            self.on_game_changed(g)
        self.load()
        self.say(f"game folder: {g.root}")

    def install(self) -> None:
        g, src = self.game, self.source
        if not g:
            messagebox.showerror("SWSE Setup", "Choose the game folder first (Browse…).")
            return
        if not src:
            messagebox.showerror("SWSE Setup", "No SWSE release beside this program.\n\n"
                                 "Run it from the unzipped SWSE download - the folder that "
                                 "holds bin\\ and SWSEMods\\.")
            return
        if sf.game_running(g.exe):
            messagebox.showinfo("SWSE Setup", "Close the game first: Windows keeps SWSE's DLL "
                                "locked while it runs.\n\nTo change only the switches now, use "
                                "Apply switches.")
            return
        if not messagebox.askyesno(
                "SWSE Setup",
                f"Install SWSE into\n{g.root}?\n\n"
                "Your own files stay as they are: binds, aliases, positions, sites, prefs and "
                "playerprefs, features.txt (it gets your ticks), load_order.txt, an aiprefs.txt "
                "that is not the shipped one, and any mod folder you added.\n\n"
                "A shipped file you changed is replaced; your copy goes to SWSE-backup-<date> in "
                "the game folder."):
            return
        rep = si.install(src, g, self.states(), {p.name: p.description for p in self.plugins})
        for line in rep.lines:
            self.say(line)
        if rep.ok:
            self.say("Installed. Start the game; the console is the ~ key.")
            self.load()
            # HD textures on: the pack's state, and the 4GB patch if it is due
            self.hd_status(offer=True)

    def apply(self) -> None:
        g = self.game
        if not g:
            messagebox.showerror("SWSE Setup", "Choose the game folder first (Browse…).")
            return
        if not g.mods.is_dir():
            messagebox.showinfo("SWSE Setup", "SWSE is not installed in this folder yet - "
                                "Install / Update puts it in with these switches.")
            return
        path = g.mods / "features.txt"
        before = sf.parse(sf.read_text(path))
        old = dict(before.states)
        old.update({p.name: before.plugin_state(p.name) for p in self.plugins})
        states = self.states()
        sf.save(path, states, {p.name: p.description for p in self.plugins})
        changed = [n for n in states if states[n] != old.get(n)]
        on = [n for n, v in states.items() if v == "on"]
        self.say("features.txt saved - on: " + (", ".join(on) or "nothing"))
        if "hdtextures" in changed:
            self.hd_status(offer=True)
        if not changed:
            self.say("(nothing changed)")
            return
        if not sf.game_running(g.exe):
            self.say("The game must be restarted to use them - or type `features <name> on` "
                     "in the console (~) to switch one live.")
            return
        lines = [f"features {n} {states[n]}" for n in changed]
        self.say("The game is running - switching them now through the console mailbox: "
                 + "; ".join(lines))

        def work() -> None:
            reply = sf.mailbox_send(g.mods, lines, timeout=6.0)

            def done() -> None:
                if reply is None:
                    self.say("The game has not answered yet: it reads the mailbox only while "
                             "it draws frames. Switch to it and they apply then - features.txt "
                             "is saved either way.")
                    return
                for ln in reply.splitlines():
                    if ln.strip():
                        self.say("  " + ln)
            self.after(0, done)

        threading.Thread(target=work, daemon=True).start()

    def uninstall(self) -> None:
        g = self.game
        if not g:
            return
        if sf.game_running(g.exe):
            messagebox.showinfo("SWSE Setup", "Close the game first: Windows keeps SWSE's DLL locked.")
            return
        if not messagebox.askyesno(
                "SWSE Setup",
                f"Take SWSE out of\n{g.root}?\n\nbin\\dinput8.dll and the SWSEMods folder are moved "
                "into a dated SWSE-uninstalled folder in the game folder - nothing is deleted, and "
                "the game runs as shipped."):
            return
        rep = si.uninstall(g)
        for line in rep.lines:
            self.say(line)
        self.load()


# ---------------------------------------------------------------- --selftest
def run_selftest(report: str, build: Callable[[], tuple[tk.Tk, "SetupPanel", str]]) -> int:
    """`<app> --selftest <file>`: `build()` makes the window and returns
    (root, panel, a line saying what was built); the root is withdrawn before
    any event is processed, so it is never shown. Checks what the Setup screen
    stands on, writes the answers to <file>; 0 when all is well."""
    lines: list[str] = []
    ok = True
    try:
        root, panel, what = build()
        root.withdraw()
        lines.append(what)
        g = panel.game
        lines.append(f"game: {g.root} ({g.store})" if g else "game: not found")
        src = panel.source
        lines.append(f"release beside it: {src or 'none'}")
        if src:
            ff = sf.parse(sf.read_text(src / "SWSEMods" / "features.txt"))
            lines.append("shipped features.txt: " + " ".join(f"{k}={v}" for k, v in ff.states.items()))
            ok = ok and ff.states == {s.name: s.default for s in sf.BUILTINS}
        lines.append(f"switch rows: {len(panel.vars)} ({len(panel.plugins)} plugin(s))")
        ok = ok and len(panel.vars) >= len(sf.BUILTINS)
        root.destroy()
    except Exception as e:  # noqa: BLE001
        ok = False
        lines.append(f"FAILED: {e!r}")
    lines.append("selftest " + ("OK" if ok else "FAILED"))
    Path(report).write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0 if ok else 1
