"""Mod Loader - the modding UI for Oddworld: Stranger's Wrath HD.

v0.2:
- Textures tab: browse, preview, replace DXT1 textures with your own images
- Values tab: edit weapon/NPC stat blocks (labeled fields where known; every
  field is editable so you can discover what unlabeled ones do)
- Save writes a modded archive into the game with automatic backup

Run:  python studio.py
Build .exe:  python -m PyInstaller --onefile --windowed --name ModLoader studio.py
"""
from __future__ import annotations

import shutil
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from PIL import Image, ImageTk

from oddforge import __version__
from oddforge.container import SmbContainer
from oddforge.dump import export_all, export_archive
from oddforge.modloader import ModLoader
from oddforge.resize import resize_entry
from oddforge.textures import decode_entry, replace_entry
from oddforge.toc import TextureEntry, find_textures

DEFAULT_GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath\data")
GOLD = "#d6a854"
DARK = "#1c1a17"
PANEL = "#26231f"
TEXT = "#e8e2d4"
DIM = "#8d876f"


class Studio(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title(f"Mod Loader v{__version__} - Stranger's Wrath modding")
        self.geometry("1080x680")
        self.configure(bg=DARK)

        self.container: SmbContainer | None = None
        self.archive_path: Path | None = None
        self.tex_entries: list[TextureEntry] = []
        self.pending_tex: dict[str, Image.Image] = {}
        self.pending_upres: dict[str, tuple[Image.Image, int, int]] = {}
        self.pending_vals = 0
        self._preview_ref: ImageTk.PhotoImage | None = None
        self.records: list = []
        self._rec_vars: list[tuple] = []

        self._build_ui()

    # ------------------------------------------------------------------ UI
    def _build_ui(self) -> None:
        style = ttk.Style(self)
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

        top = tk.Frame(self, bg=DARK)
        top.pack(fill="x", padx=10, pady=8)
        tk.Button(top, text="Open Archive…", command=self.open_archive,
                  bg=GOLD, fg=DARK, relief="flat", font=("Segoe UI", 10, "bold"),
                  padx=12).pack(side="left")
        qo = tk.Menubutton(top, text="Quick Open ▾", bg=PANEL, fg=GOLD, relief="flat",
                           font=("Segoe UI", 10, "bold"), padx=12)
        menu = tk.Menu(qo, tearoff=0, bg=PANEL, fg=TEXT,
                       activebackground=GOLD, activeforeground=DARK)
        qo.configure(menu=menu)
        stats_menu = tk.Menu(menu, tearoff=0, bg=PANEL, fg=TEXT,
                             activebackground=GOLD, activeforeground=DARK)
        menu.add_cascade(label="Weapon stats (per region)", menu=stats_menu)
        region_names = {
            "region_00": "Region 00 - Tutorial", "region_01": "Region 01 - First town",
            "region_02": "Region 02", "region_02a": "Region 02a",
            "region_03": "Region 03", "region_04": "Region 04",
            "region_05": "Region 05", "region_06": "Region 06 - Endgame",
        }
        for reg, label in region_names.items():
            lvl = reg.replace("region", "lm_level").replace("lm_level_0", "lm_level_0")
            p = DEFAULT_GAME / "bundles" / reg / f"lm_level_{reg.split('_', 1)[1]}" \
                / f"lm_level_{reg.split('_', 1)[1]}_tgl.smb"
            stats_menu.add_command(label=label,
                                   command=lambda p=p: self._open_path(p))
        gl_menu = tk.Menu(menu, tearoff=0, bg=PANEL, fg=TEXT,
                          activebackground=GOLD, activeforeground=DARK)
        menu.add_cascade(label="Global archives", menu=gl_menu)
        for gname in ("global_prefs.smb", "global_player.smb", "global_stranger.smb",
                      "global_appglobal.smb", "global_critterpool.smb",
                      "global_ingameglobal.smb"):
            gp = DEFAULT_GAME / "global" / gname
            gl_menu.add_command(label=gname, command=lambda p=gp: self._open_path(p))
        qo.pack(side="left", padx=(8, 0))
        self.status = tk.Label(top, text="Open a .smb archive to begin.",
                               bg=DARK, fg=TEXT, font=("Segoe UI", 10))
        self.status.pack(side="left", padx=14)
        tk.Button(top, text="Save Modded Archive", command=self.save_archive,
                  bg=PANEL, fg=GOLD, relief="flat", font=("Segoe UI", 10, "bold"),
                  padx=12).pack(side="right")

        self.nb = ttk.Notebook(self)
        self.nb.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        self._build_textures_tab()
        self._build_records_tab()
        self._build_mods_tab()

    # ---------------- records tab (labeled game data)
    def _build_records_tab(self) -> None:
        tab = tk.Frame(self.nb, bg=DARK)
        self.nb.add(tab, text="  Game Data  ")

        bar = tk.Frame(tab, bg=DARK)
        bar.pack(fill="x", pady=(4, 2))
        tk.Label(bar, text="Entity:", bg=DARK, fg=TEXT).pack(side="left", padx=(4, 4))
        # one-click filters for the things people actually want to edit
        for label, term in (("All", ""), ("Steef", "steef"), ("Stranger", "stranger"),
                            ("Outlaws", "outlaw"), ("Wolvark", "wolvark"),
                            ("Sleg", "sleg"), ("Critters", "critter"),
                            ("Chicken/Fowl", "chicken"), ("Weapons", "weapon"),
                            ("Bounties", "bounty")):
            tk.Button(bar, text=label, bg=PANEL, fg=TEXT, relief="groove",
                      font=("Segoe UI", 8), padx=6,
                      command=lambda t=term: self._quick_filter(t)).pack(side="left", padx=2)

        bar2 = tk.Frame(tab, bg=DARK)
        bar2.pack(fill="x", pady=(0, 4))
        tk.Label(bar2, text="Search:", bg=DARK, fg=TEXT).pack(side="left", padx=(4, 4))
        self.rec_filter = tk.Entry(bar2, bg=PANEL, fg=TEXT, insertbackground=TEXT,
                                   relief="flat", width=30)
        self.rec_filter.pack(side="left")
        self.rec_filter.bind("<Return>", lambda e: self._refresh_records())
        tk.Button(bar2, text="Go", command=self._refresh_records, bg=PANEL,
                  fg=TEXT, relief="groove", padx=8).pack(side="left", padx=6)
        tk.Label(bar2, text="tip: open global_prefs.smb (Quick Open) for bounty/NPC data",
                 bg=DARK, fg=DIM, font=("Segoe UI", 8)).pack(side="left", padx=8)

        body = tk.Frame(tab, bg=DARK)
        body.pack(fill="both", expand=True)

        left = tk.Frame(body, bg=DARK)
        left.pack(side="left", fill="both", expand=True)
        self.rec_list = tk.Listbox(left, bg=PANEL, fg=TEXT, selectbackground=GOLD,
                                   selectforeground=DARK, relief="flat",
                                   font=("Consolas", 9))
        self.rec_list.pack(side="left", fill="both", expand=True)
        rsb = ttk.Scrollbar(left, orient="vertical", command=self.rec_list.yview)
        rsb.pack(side="right", fill="y")
        self.rec_list.configure(yscrollcommand=rsb.set)
        self.rec_list.bind("<<ListboxSelect>>", self._on_record_select)

        right = tk.Frame(body, bg=PANEL, width=460)
        right.pack(side="right", fill="both")
        right.pack_propagate(False)
        tk.Label(right, text="Fields", bg=PANEL, fg=GOLD,
                 font=("Segoe UI", 10, "bold")).pack(anchor="w", padx=8, pady=(6, 2))
        self.rec_info = tk.Label(right, bg=PANEL, fg=DIM, justify="left",
                                 font=("Consolas", 8), wraplength=440)
        self.rec_info.pack(anchor="w", padx=8)

        canvas = tk.Canvas(right, bg=PANEL, highlightthickness=0)
        canvas.pack(side="left", fill="both", expand=True, padx=(8, 0), pady=6)
        fsb = ttk.Scrollbar(right, orient="vertical", command=canvas.yview)
        fsb.pack(side="right", fill="y")
        canvas.configure(yscrollcommand=fsb.set)
        self.rec_fields_frame = tk.Frame(canvas, bg=PANEL)
        canvas.create_window((0, 0), window=self.rec_fields_frame, anchor="nw")
        self.rec_fields_frame.bind(
            "<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))

        tk.Button(right, text="Apply Field Changes", command=self._apply_record_edits,
                  bg=GOLD, fg=DARK, relief="flat", font=("Segoe UI", 10, "bold"),
                  padx=10).pack(side="bottom", pady=8)

    def _quick_filter(self, term: str) -> None:
        self.rec_filter.delete(0, "end")
        if term:
            self.rec_filter.insert(0, term)
        self._refresh_records()

    def _refresh_records(self) -> None:
        self.rec_list.delete(0, "end")
        self.records = []
        if self.container is None:
            self.rec_list.insert("end", "  (open an archive first)")
            return
        try:
            from oddforge.records import find_records, guess_class
            recs = [r for r in find_records(self.container) if r.values]
        except Exception as e:  # noqa: BLE001
            self.rec_list.insert("end", f"  (error: {e})")
            return
        filt = self.rec_filter.get().strip().lower()
        for r in recs:
            if filt and filt not in r.name.lower():
                continue
            self.records.append(r)
            cls = guess_class(r)
            tag = f"[{cls}] " if cls else ""
            self.rec_list.insert("end", f" {tag}{r.name[:70]}  ({len(r.values)})")
        if not self.records:
            self.rec_list.insert("end", "  (no matching records)")

    def _on_record_select(self, _evt=None) -> None:
        for w in self.rec_fields_frame.winfo_children():
            w.destroy()
        self._rec_vars = []
        sel = self.rec_list.curselection()
        if not sel or sel[0] >= len(self.records):
            return
        from oddforge.records import guess_class, label_record, load_schema
        r = self.records[sel[0]]
        cls = guess_class(r)
        info = f"{r.name}\nclass: {cls or 'unknown'}   values: {len(r.values)}"
        if cls:
            n = len(load_schema().get(cls, []))
            info += f"  ({cls} defines {n} fields)"
            if n > len(r.values):
                info += "\nNOTE: partial record - names are best-effort, verify in game"
        else:
            info += "\n(no class match - values shown by raw offset, still editable)"
        self.rec_info.config(text=info)
        for off, kind, val, fname in label_record(r):
            row = tk.Frame(self.rec_fields_frame, bg=PANEL)
            row.pack(fill="x", pady=1)
            label = fname or f"0x{off:06X}"
            colour = TEXT if fname else DIM
            tk.Label(row, text=label, bg=PANEL, fg=colour, width=32, anchor="w",
                     font=("Consolas", 8)).pack(side="left")
            var = tk.StringVar(value=(f"{val:.9g}" if kind == "f" else str(val)))
            tk.Entry(row, textvariable=var, bg=DARK, fg=TEXT, width=14,
                     insertbackground=TEXT, relief="flat",
                     font=("Consolas", 8)).pack(side="left", padx=4)
            self._rec_vars.append((off, kind, var))

    def _apply_record_edits(self) -> None:
        if self.container is None or not getattr(self, "_rec_vars", None):
            return
        import struct as _s
        toc = bytearray(self.container.toc)
        changed = 0
        for off, kind, var in self._rec_vars:
            try:
                new = (_s.pack("<f", float(var.get())) if kind == "f"
                       else _s.pack("<I", int(float(var.get())) & 0xFFFFFFFF))
            except ValueError:
                continue
            if toc[off:off + 4] != new:
                toc[off:off + 4] = new
                changed += 1
        if not changed:
            messagebox.showinfo("SWSE", "No field values changed.")
            return
        self.container.toc = bytes(toc)
        self.pending_vals += changed
        messagebox.showinfo("SWSE",
                            f"Staged {changed} field change(s).\n"
                            "Use 'Save Modded Archive' to write them to the game.")

    # ---------------- textures tab
    def _build_textures_tab(self) -> None:
        tab = tk.Frame(self.nb, bg=DARK)
        self.nb.add(tab, text="  Textures  ")

        left = tk.Frame(tab, bg=DARK)
        left.pack(side="left", fill="both", expand=True)
        cols = ("name", "size", "fmt", "status")
        self.tex_tree = ttk.Treeview(left, columns=cols, show="headings", selectmode="browse")
        for c, w in zip(cols, (300, 90, 70, 90)):
            self.tex_tree.heading(c, text=c.title())
            self.tex_tree.column(c, width=w, anchor="w")
        self.tex_tree.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(left, orient="vertical", command=self.tex_tree.yview)
        sb.pack(side="right", fill="y")
        self.tex_tree.configure(yscrollcommand=sb.set)
        self.tex_tree.bind("<<TreeviewSelect>>", self._on_tex_select)

        right = tk.Frame(tab, bg=PANEL, width=340)
        right.pack(side="right", fill="y")
        right.pack_propagate(False)
        self.preview = tk.Label(right, bg=PANEL, text="(preview)", fg=TEXT)
        self.preview.pack(pady=16)
        self.tex_info = tk.Label(right, bg=PANEL, fg=TEXT, justify="left",
                                 font=("Consolas", 9))
        self.tex_info.pack(pady=4)
        self.replace_btn = tk.Button(right, text="Replace with image…",
                                     command=self.replace_selected_texture,
                                     bg=GOLD, fg=DARK, relief="flat",
                                     font=("Segoe UI", 10, "bold"),
                                     state="disabled", padx=10)
        self.replace_btn.pack(pady=(10, 2))
        self.export_one_btn = tk.Button(right, text="Export selected as PNG…",
                                        command=self.export_selected_texture,
                                        bg=PANEL, fg=TEXT, relief="groove",
                                        state="disabled", padx=10)
        self.export_one_btn.pack(pady=2)

        # ---- texture dump (export for AI upscaling) ----
        tk.Label(right, text="Texture dump", bg=PANEL, fg=GOLD,
                 font=("Segoe UI", 10, "bold")).pack(pady=(18, 2))
        tk.Label(right, bg=PANEL, fg=DIM, justify="left", wraplength=300,
                 font=("Segoe UI", 8),
                 text=("Export PNGs, AI-upscale them, then copy the folder "
                       "into SWSEMods\\<YourMod>\\textures\\ and use "
                       "APPLY ALL MODS to reimport.")).pack(padx=8)
        self.export_arch_btn = tk.Button(right, text="Export this archive…",
                                         command=self.export_archive_textures,
                                         bg=PANEL, fg=TEXT, relief="groove",
                                         state="disabled", padx=10)
        self.export_arch_btn.pack(pady=(8, 2))
        tk.Button(right, text="Export ALL game textures…",
                  command=self.export_all_textures,
                  bg=PANEL, fg=TEXT, relief="groove", padx=10).pack(pady=2)
        self.export_status = tk.Label(right, bg=PANEL, fg=DIM,
                                      font=("Consolas", 8))
        self.export_status.pack(pady=4)

    # ---------------- values tab

    # ---------------- mod loader tab
    def _build_mods_tab(self) -> None:
        tab = tk.Frame(self.nb, bg=DARK)
        self.nb.add(tab, text="  Mod Loader  ")
        game_root = DEFAULT_GAME.parent
        self.loader = ModLoader(game_data=DEFAULT_GAME,
                                mods_root=game_root / "SWSEMods")

        left = tk.Frame(tab, bg=DARK)
        left.pack(side="left", fill="y", padx=(0, 10))
        tk.Label(left, text="Installed mods (top loads first)", bg=DARK, fg=GOLD,
                 font=("Segoe UI", 10, "bold")).pack(anchor="w", pady=(4, 2))
        self.mods_list = tk.Listbox(left, bg=PANEL, fg=TEXT, width=42,
                                    selectbackground=GOLD, selectforeground=DARK,
                                    relief="flat", font=("Consolas", 10))
        self.mods_list.pack(fill="y", expand=True)
        self.mods_list.bind("<<ListboxSelect>>", self._on_mod_select)
        btns = tk.Frame(left, bg=DARK)
        btns.pack(fill="x", pady=6)
        for label, cmd in (("▲", lambda: self._move_mod(-1)),
                           ("▼", lambda: self._move_mod(1)),
                           ("Enable/Disable", self._toggle_mod),
                           ("Refresh", self._refresh_mods)):
            tk.Button(btns, text=label, command=cmd, bg=PANEL, fg=TEXT,
                      relief="flat", padx=8).pack(side="left", padx=2)

        right = tk.Frame(tab, bg=PANEL)
        right.pack(side="left", fill="both", expand=True)
        self.mod_desc = tk.Label(right, bg=PANEL, fg=TEXT, justify="left",
                                 wraplength=460, anchor="nw",
                                 font=("Segoe UI", 10), height=5)
        self.mod_desc.pack(fill="x", padx=10, pady=(10, 4))
        self.mods_log = tk.Text(right, bg=DARK, fg=TEXT, relief="flat",
                                font=("Consolas", 10), height=13, wrap="word")
        self.mods_log.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        row = tk.Frame(right, bg=PANEL)
        row.pack(fill="x", padx=10, pady=(0, 10))
        tk.Button(row, text="APPLY ALL MODS", command=self._apply_mods,
                  bg=GOLD, fg=DARK, relief="flat", font=("Segoe UI", 11, "bold"),
                  padx=16, pady=4).pack(side="left")
        tk.Button(row, text="Revert to Vanilla", command=self._revert_mods,
                  bg=PANEL, fg=GOLD, relief="flat", font=("Segoe UI", 10, "bold"),
                  padx=12).pack(side="left", padx=8)
        tk.Button(row, text="Open Mods Folder", command=self._open_mods_folder,
                  bg=PANEL, fg=TEXT, relief="flat", padx=12).pack(side="right")
        self._refresh_mods()

    def _log(self, msg: str) -> None:
        self.mods_log.insert("end", msg + "\n")
        self.mods_log.see("end")
        self.update_idletasks()

    def _refresh_mods(self) -> None:
        self.mods_list.delete(0, "end")
        self._mods = self.loader.discover()
        for m in self._mods:
            box = "[x]" if m.enabled else "[ ]"
            self.mods_list.insert("end", f"{box} {m.name}  v{m.version}")
            if not m.enabled:
                self.mods_list.itemconfig("end", fg=DIM)

    def _on_mod_select(self, _evt=None) -> None:
        sel = self.mods_list.curselection()
        if not sel:
            return
        m = self._mods[sel[0]]
        state = "ENABLED" if m.enabled else "DISABLED"
        self.mod_desc.config(
            text=f"{m.name}  v{m.version} - by {m.author}   [{state}]\n\n"
                 f"{m.description or '(no description in mod.json)'}")

    def _toggle_mod(self) -> None:
        sel = self.mods_list.curselection()
        if not sel:
            return
        i = sel[0]
        m = self._mods[i]
        self.loader.set_enabled(m.folder.name, not m.enabled)
        self._refresh_mods()
        self.mods_list.selection_set(i)
        self._on_mod_select()

    def _move_mod(self, delta: int) -> None:
        sel = self.mods_list.curselection()
        if not sel:
            return
        i = sel[0]
        j = i + delta
        if not (0 <= j < len(self._mods)):
            return
        self._mods[i], self._mods[j] = self._mods[j], self._mods[i]
        self.loader.write_load_order(self._mods)
        self._refresh_mods()
        self.mods_list.selection_set(j)

    def _apply_mods(self) -> None:
        self._log("Applying mods (first run builds the texture index - slow once)…")
        try:
            summary = self.loader.apply()
        except Exception as e:  # noqa: BLE001
            self._log(f"ERROR: {e}")
            return
        self._log(f"Applied: {', '.join(summary['mods']) or '(none)'}")
        for rel, counts in summary["archives"].items():
            self._log(f"  {rel}: {counts['textures']} texture(s), {counts['values']} value(s)")
        for w in summary["warnings"]:
            self._log(f"  [warn] {w}")
        self._log("Done. Launch the game to see your mods.\n")

    def _revert_mods(self) -> None:
        restored = self.loader.revert()
        self._log(f"Reverted {len(restored)} archive(s) to vanilla.\n")

    def _open_mods_folder(self) -> None:
        import os
        self.loader.mods_root.mkdir(parents=True, exist_ok=True)
        os.startfile(self.loader.mods_root)  # noqa: S606

    # ------------------------------------------------------------------ shared
    def open_archive(self) -> None:
        start = DEFAULT_GAME if DEFAULT_GAME.exists() else Path.home()
        path = filedialog.askopenfilename(
            title="Open SMB archive", initialdir=str(start),
            filetypes=[("SMB archives", "*.smb"), ("All files", "*.*")])
        if not path:
            return
        self._open_path(Path(path))

    def _open_path(self, path: Path) -> None:
        if not path.exists():
            messagebox.showerror("SWSE", f"Not found:\n{path}")
            return
        self.archive_path = Path(path)
        try:
            self.container = SmbContainer.parse_file(self.archive_path)
            self.tex_entries = find_textures(self.container)
        except Exception as e:  # noqa: BLE001
            messagebox.showerror("SWSE", f"Could not read archive:\n{e}")
            return
        self.pending_tex.clear()
        self.pending_upres.clear()
        self.pending_vals = 0
        self._refresh_tex_tree()
        self._refresh_records()
        self.export_arch_btn.config(state="normal")
        n_dxt1 = sum(t.is_dxt1 for t in self.tex_entries)
        self.status.config(text=f"{self.archive_path.name} - {len(self.tex_entries)} textures "
                                f"({n_dxt1} editable)")

    # ---------------- texture dump (export)
    def export_archive_textures(self) -> None:
        if self.archive_path is None:
            return
        out = filedialog.askdirectory(title="Export textures to folder…")
        if not out:
            return
        try:
            e, s = export_archive(self.archive_path, out)
        except Exception as exc:  # noqa: BLE001
            messagebox.showerror("SWSE", f"Export failed:\n{exc}")
            return
        self.export_status.config(text=f"exported {e}, skipped {s}")
        messagebox.showinfo("SWSE", f"Exported {e} textures to:\n{out}\n"
                                        f"({s} skipped - not DXT1)")

    def export_all_textures(self) -> None:
        if not DEFAULT_GAME.exists():
            messagebox.showerror("SWSE", f"Game data not found:\n{DEFAULT_GAME}")
            return
        out = filedialog.askdirectory(title="Export ALL game textures to folder…")
        if not out:
            return
        self.export_status.config(text="dumping…")

        def work() -> None:
            def prog(rel: str, done: int, total: int) -> None:
                if done % 20 == 0 or done == total:
                    self.after(0, lambda: self.export_status.config(
                        text=f"{done}/{total} archives…"))
            st = export_all(DEFAULT_GAME, out, progress=prog)
            def finish() -> None:
                self.export_status.config(
                    text=f"done: {st['exported']} PNGs from {st['archives']} archives")
                messagebox.showinfo(
                    "SWSE",
                    f"Dumped {st['exported']} textures from {st['archives']} archives.\n"
                    f"({st['skipped']} skipped, {st['errors']} unreadable)\n\n"
                    f"Folder: {out}\n\n"
                    "Next: AI-upscale the PNGs, then copy the folder into\n"
                    "SWSEMods\\<YourMod>\\textures\\ and APPLY ALL MODS.")
            self.after(0, finish)

        threading.Thread(target=work, daemon=True).start()

    def save_archive(self) -> None:
        if self.container is None or self.archive_path is None:
            return
        if not self.pending_tex and self.pending_vals == 0:
            messagebox.showinfo("SWSE", "No changes to save yet.")
            return
        for t in self.tex_entries:
            if t.name in self.pending_tex and t.name not in self.pending_upres:
                try:
                    replace_entry(self.container, t, self.pending_tex[t.name])
                except Exception as e:  # noqa: BLE001
                    messagebox.showerror("SWSE", f"{t.name}:\n{e}")
                    return
        # TRUE up-res (experimental): each grow shifts later offsets, so
        # re-locate the entry fresh before every resize.
        for name, (img, nw, nh) in self.pending_upres.items():
            try:
                t = next(x for x in find_textures(self.container) if x.name == name)
                resize_entry(self.container, t, img, nw, nh)
            except Exception as e:  # noqa: BLE001
                messagebox.showerror("SWSE", f"up-res {name}:\n{e}")
                return
        rebuilt = self.container.build()

        # NOTE: when frozen (PyInstaller), __file__ lives in a self-deleting
        # temp dir - backups MUST go next to the exe instead.
        if getattr(sys, "frozen", False):
            backups = Path(sys.executable).resolve().parent / "backups"
        else:
            backups = Path(__file__).resolve().parent / "backups"
        backups.mkdir(exist_ok=True)
        bak = backups / self.archive_path.name
        if not bak.exists():
            shutil.copy2(self.archive_path, bak)
        self.archive_path.write_bytes(rebuilt)
        n = len(self.pending_tex) + self.pending_vals
        n_up = len(self.pending_upres)
        extra = f" ({n_up} up-res'd)" if n_up else ""
        messagebox.showinfo("SWSE",
                            f"Saved {n} change(s){extra} to\n{self.archive_path.name}\n\n"
                            f"Original backed up to backups\\{self.archive_path.name}")
        self.pending_tex.clear()
        self.pending_upres.clear()
        self.pending_vals = 0
        self.container = SmbContainer.parse_file(self.archive_path)
        self.tex_entries = find_textures(self.container)
        self._refresh_tex_tree()

    # ------------------------------------------------------------------ textures logic
    def _refresh_tex_tree(self) -> None:
        self.tex_tree.delete(*self.tex_tree.get_children())
        for i, t in enumerate(self.tex_entries):
            name = t.name.rsplit("\\", 1)[-1]
            fmt = "DXT1" if t.is_dxt1 else f"fmt{t.fmt}"
            if t.name in self.pending_upres:
                nw, nh = self.pending_upres[t.name][1], self.pending_upres[t.name][2]
                status = f"RE-SIZE {nw}x{nh}"
            elif t.name in self.pending_tex:
                status = "MODDED"
            else:
                status = "editable" if t.is_dxt1 else "view-only"
            self.tex_tree.insert("", "end", iid=str(i),
                                 values=(name, f"{t.width}x{t.height}", fmt, status))

    def _current_texture(self) -> TextureEntry | None:
        sel = self.tex_tree.selection()
        return self.tex_entries[int(sel[0])] if sel else None

    def _on_tex_select(self, _evt=None) -> None:
        t = self._current_texture()
        if t is None or self.container is None:
            return
        self.tex_info.config(text=f"{t.name.rsplit(chr(92),1)[-1]}\n"
                                  f"{t.width} x {t.height}\nformat {t.fmt}  mips {t.mips}")
        img = (self.pending_tex[t.name].convert("RGB") if t.name in self.pending_tex
               else decode_entry(self.container, t))
        if img is None:
            self.preview.config(image="", text="(no preview for this format yet)")
            self._preview_ref = None
            self.replace_btn.config(state="disabled")
            self.export_one_btn.config(state="disabled")
            return
        disp = img.copy()
        disp.thumbnail((300, 300))
        self._preview_ref = ImageTk.PhotoImage(disp)
        self.preview.config(image=self._preview_ref, text="")
        self.replace_btn.config(state="normal" if t.is_dxt1 else "disabled")
        self.export_one_btn.config(state="normal" if t.is_dxt1 else "disabled")

    def export_selected_texture(self) -> None:
        t = self._current_texture()
        if t is None or self.container is None or not t.is_dxt1:
            return
        img = decode_entry(self.container, t)
        if img is None:
            messagebox.showerror("SWSE", "Could not decode this texture.")
            return
        default = t.name.rsplit("\\", 1)[-1].rsplit(".", 1)[0] + ".png"
        path = filedialog.asksaveasfilename(
            title="Export texture as PNG", defaultextension=".png",
            initialfile=default, filetypes=[("PNG image", "*.png")])
        if not path:
            return
        img.save(path)
        self.export_status.config(text=f"exported {Path(path).name}")

    def replace_selected_texture(self) -> None:
        t = self._current_texture()
        if t is None or not t.is_dxt1:
            return
        path = filedialog.askopenfilename(
            title=f"Choose image ({t.width}x{t.height})",
            filetypes=[("Images", "*.png *.jpg *.jpeg *.bmp *.tga"), ("All files", "*.*")])
        if not path:
            return
        try:
            img = Image.open(path)
        except Exception as e:  # noqa: BLE001
            messagebox.showerror("SWSE", f"Could not open image:\n{e}")
            return

        # bigger image? offer TRUE up-resolution (experimental archive relayout)
        self.pending_upres.pop(t.name, None)
        if img.width >= t.width * 2 and img.height >= t.height * 2:
            # snap target to the largest power-of-two multiple <= image size
            nw, nh = t.width, t.height
            while nw * 2 <= img.width and nh * 2 <= img.height and nw < 2048:
                nw, nh = nw * 2, nh * 2
            up = messagebox.askyesnocancel(
                "SWSE - up-res?",
                f"This image is {img.width}x{img.height}; the game texture is "
                f"{t.width}x{t.height}.\n\n"
                f"YES = grow the texture to {nw}x{nh} in the archive.\n"
                f"WARNING: the engine binds some textures (character models!) "
                f"to their original size elsewhere - those will render "
                f"CORRUPTED at a new size. Use only for world/level textures, "
                f"and keep a backup.\n\n"
                f"NO = fit the image to {t.width}x{t.height} (always safe, "
                f"still sharper than vanilla)\n")
            if up is None:
                return
            if up:
                self.pending_upres[t.name] = (img, nw, nh)
        elif img.width <= t.width // 2 and img.height <= t.height // 2:
            # smaller image: offer DOWN-res (e.g. return an up-res'd texture
            # to its native size). Same relayout engine, shrinking.
            down = messagebox.askyesnocancel(
                "SWSE - down-res?",
                f"This image is {img.width}x{img.height}; the game texture is "
                f"currently {t.width}x{t.height}.\n\n"
                f"YES = SHRINK the texture to {img.width}x{img.height} in the "
                f"archive (returns an up-res'd texture to a smaller size)\n"
                f"NO = stretch the image up to {t.width}x{t.height}\n")
            if down is None:
                return
            if down:
                if img.width & (img.width - 1) or img.height & (img.height - 1):
                    messagebox.showerror(
                        "SWSE", "Down-res target must be power-of-two "
                        f"dimensions; {img.width}x{img.height} is not.")
                    return
                self.pending_upres[t.name] = (img, img.width, img.height)
        self.pending_tex[t.name] = img
        self._refresh_tex_tree()
        idx = self.tex_entries.index(t)
        self.tex_tree.selection_set(str(idx))
        self._on_tex_select()




if __name__ == "__main__":
    Studio().mainloop()
