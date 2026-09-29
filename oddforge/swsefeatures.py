"""SWSE's feature switches - SWSEMods\\features.txt - for SWSE Setup.

Read and written by the rules swse/features.cpp uses, so a file SWSE Setup
saves is one SWSE reads exactly as intended, and the other way round:

  * a switch line is `name = value`, `#` or `;` starts a comment, spaces and
    tabs anywhere in the key and value are ignored, names match without case;
  * `on`, `true`, `yes`, `1` and `enabled` mean on, anything else off;
    `auto` counts only for aituning and playertune;
  * a missing line (or a missing file) means the default: console on,
    aituning and playertune auto, everything else off; a later line wins;
  * a line naming no built-in switch is a plugin's (plugins are switched by
    their DLL's name), kept as written;
  * saving rewrites only each switch line's value - indentation, spacing and a
    trailing comment stay - drops a repeated switch line, appends the switches
    the file lacks, and writes beside the file before swapping it in.

Also here: the presets (`features preset` in the console), finding plugins the
way SWSE does, reading a DLL's version resource, whether the game is running,
and the console mailbox (TOOL_CONTRACT.md section 2) for switching live.
"""
from __future__ import annotations

import os
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path


@dataclass(frozen=True)
class Switch:
    name: str
    default: str            # "on" / "off" / "auto"
    auto_capable: bool
    oneliner: str           # the trailing comment on its line in the shipped features.txt
    cpp_desc: str           # features.cpp kDesc: what SWSE writes above an appended line


# In features.cpp's order. `oneliner` is the shipped features.txt's own short
# comment for the switch (tools/test_swsefeatures.py checks they still match);
# `cpp_desc` is features.cpp's kDesc, used the way SWSE uses it.
BUILTINS: tuple[Switch, ...] = (
    Switch("console", "on", False, "in-game console (~ key) + remote mailbox",
           "in-game console (~) and the remote command mailbox"),
    Switch("graphics", "off", False, "post-process look (F10 once on)",
           "post-process pipeline: AO, RTGI, bloom, grade (F10 toggles the look)"),
    Switch("hdtextures", "off", False, "HD texture pack (needs the 4GB patch)",
           "HD texture replacement (.oft files swapped in at upload)"),
    Switch("hitreact", "off", False, "NPCs flinch where they are shot",
           "additive hit reactions - NPCs flinch where they are shot"),
    Switch("foliage", "off", False, "grass and plants sway in the wind",
           "foliage wind: grass and plants sway and part around you"),
    Switch("aituning", "auto", True, "enemy AI profiles from aiprefs.txt",
           "aiprefs.txt enemy tuning: sight, fire rate, reload, accuracy, miss time"),
    Switch("triggers", "off", False, "mod-defined events (ambushes)",
           "triggers.txt mod events (ambushes and other reactions)"),
    Switch("npctuning", "off", False, "per-character health / gib rules",
           "characters.txt / console.txt per-character health and gib, every level"),
    Switch("playertune", "auto", True, "your health, stamina, speed, jump...",
           "playerprefs.txt: player health, stamina, speed, jump, every level"),
    Switch("prefsedit", "off", False, "live edits to the game's own values",
           "prefs.txt: live edits to any loaded prefs record, every level"),
    Switch("raytrace", "off", False, "ray-traced AO, EXPERIMENTAL (needs graphics)",
           "ray-traced ambient occlusion - EXPERIMENTAL; needs graphics on"),
)
BUILTIN_NAMES = tuple(s.name for s in BUILTINS)
_BY_NAME = {s.name: s for s in BUILTINS}

# features.cpp kAlias: nicknames the console accepts. A plugin may not take one.
ALIASES = ("grass", "wind", "plants", "hd", "textures", "gfx", "postfx", "rtgi", "ai",
           "difficulty", "ambushes", "characters", "tuning", "player", "playerprefs",
           "prefs", "rt", "rtao", "raytracing", "combat", "hitreactions")

# Switched on in the middle of a game, these take full effect from the next
# level load (the shipped features.txt; `features preset` says so too).
NEXT_LEVEL = ("hdtextures", "hitreact", "foliage")

TRUTHY = ("on", "true", "yes", "1", "enabled")

# Presets. The console's `features preset full|default` and SWSE Setup's
# buttons. None = left as it is.
PRESETS: dict[str, dict[str, str | None]] = {
    # the 1.1 default: console on, the auto pair auto, everything else off
    # (plugins too - they are off unless named)
    "default": {**{s.name: s.default for s in BUILTINS}, "*plugins": "off"},
    # the 1.0.x look: what 1.0.x switched on by default, bar triggers
    "full": {"console": "on", "graphics": "on", "hdtextures": "on", "hitreact": "on",
             "foliage": "on", "aituning": "on"},
    # every built-in on, like `features all on`: never a plugin
    "everything": {s.name: "on" for s in BUILTINS},
}

HEADER_IF_NEW = ("# SWSE - feature switches (written by SWSE Setup)\r\n"
                 "# Only the console is on unless a line below says otherwise.\r\n\r\n")


def valid_switch_name(name: str) -> bool:
    """features.cpp ValidKeyName / plugins.cpp ValidPluginName."""
    return 1 <= len(name) <= 31 and all(c in "abcdefghijklmnopqrstuvwxyz0123456789_-" for c in name)


def reserved(name: str) -> bool:
    """features.cpp SWSE_FeatureNameReserved: a plugin cannot take it."""
    n = name.lower()
    return n in BUILTIN_NAMES or n in ALIASES or n in ("all", "preset")


# ---------------------------------------------------------------- the file
def parse_line(line: str) -> tuple[str, str] | None:
    """features.cpp ParseLine: (key, value) of a switch line, else None."""
    s = line.lstrip(" \t")
    if not s or s[0] in "#;\r\n":
        return None
    key, val, eq = [], [], False
    for c in s:
        if c in "#;\r\n":
            break
        if c == "=":
            eq = True
            continue
        if c in " \t":
            continue
        if not eq:
            if len(key) < 31:
                key.append(c)
        elif len(val) < 31:
            val.append(c)
    k, v = "".join(key), "".join(val)
    return (k, v) if k and v else None


def truthy(v: str) -> bool:
    return v.lower() in TRUTHY


def read_text(path: Path) -> str | None:
    """The file as text (bytes kept 1:1 through latin-1), or None if missing."""
    try:
        return Path(path).read_bytes().decode("latin-1")
    except OSError:
        return None


@dataclass
class FeatureFile:
    states: dict[str, str]                  # every built-in: "on" / "off" / "auto"
    others: dict[str, bool] = field(default_factory=dict)   # lowercased key -> on (plugins' lines)
    from_file: bool = False

    def plugin_state(self, name: str) -> str:
        return "on" if self.others.get(name.lower(), False) else "off"


def parse(text: str | None) -> FeatureFile:
    """features.cpp SWSE_FeaturesInit: the defaults, then every switch line."""
    states = {s.name: s.default for s in BUILTINS}
    ff = FeatureFile(states=states, from_file=text is not None)
    if text is None:
        return ff
    for line in text.split("\n"):
        kv = parse_line(line)
        if not kv:
            continue
        key, val = kv
        sw = _BY_NAME.get(key.lower())
        if sw:
            states[sw.name] = "auto" if (val.lower() == "auto" and sw.auto_capable) \
                else ("on" if truthy(val) else "off")
        else:
            ff.others[key.lower()] = truthy(val)       # a later line wins
    return ff


def _rewrite_value(line: str, value: str) -> str:
    """features.cpp RewriteValue: only the value changes."""
    eq = line.find("=")
    if eq < 0:
        return line
    vs = eq + 1
    while vs < len(line) and line[vs] in " \t":
        vs += 1
    ve = vs
    while ve < len(line) and line[ve] not in " \t#;\r":
        ve += 1
    return line[:vs] + value + line[ve:]


def render(original: str | None, states: dict[str, str],
           plugin_descs: dict[str, str] | None = None) -> str:
    """features.cpp SWSE_FeaturesSave, as text (CRLF).

    `states` maps every switch to write - the built-ins and the plugins found -
    to "on" / "off" / "auto". `plugin_descs` gives a plugin's description for
    the comment above an appended line.
    """
    plugin_descs = plugin_descs or {}
    order = list(BUILTIN_NAMES) + [n for n in states if n not in _BY_NAME]
    known = {n.lower(): n for n in order}
    text = original or ""
    out: list[str] = []
    if not text:
        out.append(HEADER_IF_NEW)
    written: set[str] = set()
    pos, n = 0, len(text)
    while pos < n:
        nl = text.find("\n", pos)
        had_nl = nl >= 0
        line = text[pos:nl] if had_nl else text[pos:]
        pos = nl + 1 if had_nl else n
        if line.endswith("\r"):
            line = line[:-1]
        kv = parse_line(line)
        name = known.get(kv[0].lower()) if kv else None
        if name:
            if name in written:
                continue                        # drop a repeated switch line, newline and all
            out.append(_rewrite_value(line, states[name]))
            written.add(name)
        else:
            out.append(line)
        if had_nl or pos < n:
            out.append("\r\n")
    for name in order:
        if name in written:
            continue
        if name in _BY_NAME:
            desc = _BY_NAME[name].cpp_desc
        elif plugin_descs.get(name):
            desc = f"plugin: {plugin_descs[name]}"
        else:
            desc = f"plugin {name}"
        out.append(f"\r\n# {desc}\r\n{name:<10} = {states[name]}\r\n")
    return "".join(out)


def save(path: Path, states: dict[str, str], plugin_descs: dict[str, str] | None = None) -> str:
    """Rewrite features.txt with `states`: beside, then swapped in. Returns the text."""
    path = Path(path)
    new = render(read_text(path), states, plugin_descs)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(new.encode("latin-1", errors="replace"))
    os.replace(tmp, path)
    return new


def explanations(text: str | None) -> dict[str, str]:
    """The shipped file's long explanation of each built-in switch: the
    comment block that starts `# <name> - `, up to a line that is not a
    comment or is a bare `#`."""
    out: dict[str, str] = {}
    if not text:
        return out
    cur: str | None = None
    buf: list[str] = []

    def flush() -> None:
        if cur and buf:
            out.setdefault(cur, " ".join(buf))

    for raw in text.replace("\r", "").split("\n"):
        s = raw.strip()
        body = s[1:].strip() if s.startswith("#") else None
        start = None
        if body is not None:
            head = body.split(" - ", 1)
            if len(head) == 2 and head[0].strip().lower() in _BY_NAME:
                start = (head[0].strip().lower(), head[1].strip())
        if start:
            flush()
            cur, buf = start[0], [start[1]]
        elif cur and body:                      # a comment line that carries text
            buf.append(body)
        else:
            flush()
            cur, buf = None, []
    flush()
    return out


def apply_preset(states: dict[str, str], preset: str, plugins: list[str] | tuple[str, ...] = ()) -> dict[str, str]:
    """A copy of `states` with a preset's values laid over it."""
    p = PRESETS[preset]
    out = dict(states)
    for name, value in p.items():
        if name == "*plugins":
            for pl in plugins:
                out[pl] = value
        elif value is not None:
            out[name] = value
    return out


# ---------------------------------------------------------------- plugins
@dataclass
class Plugin:
    name: str               # the switch: the DLL's file name, lower case
    mod: str                # the mod folder it comes from
    path: Path
    description: str = ""
    version: str = ""


def read_load_order(mods_root: Path) -> tuple[list[str], set[str]]:
    """modregistry.cpp ReadLoadOrder: (listed names, disabled names), lower-cased."""
    try:
        raw = (Path(mods_root) / "load_order.txt").read_bytes()
    except OSError:
        return [], set()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    order, disabled = [], set()
    for line in raw.decode("latin-1").split("\n"):
        s = line.strip(" \t\r")
        if not s or s[0] in "#;":
            continue
        off = s.startswith("!")
        if off:
            s = s[1:].lstrip(" ")
        if not s:
            continue
        if off:
            disabled.add(s.lower())
        order.append(s.lower())
    return order, disabled


def enabled_mods(mods_root: Path) -> list[Path]:
    """The enabled mod folders in SWSE's load order: listed ones as listed,
    then the rest alphabetically; a folder starting with '.' is not a mod."""
    root = Path(mods_root)
    try:
        dirs = [d for d in root.iterdir() if d.is_dir() and not d.name.startswith(".")]
    except OSError:
        return []
    order, disabled = read_load_order(root)
    index = {n: i for i, n in reversed(list(enumerate(order)))}

    def key(d: Path):
        i = index.get(d.name.lower())
        return (0, i, "") if i is not None else (1, 0, d.name.lower())

    return [d for d in sorted(dirs, key=key) if d.name.lower() not in disabled]


def discover_plugins(mods_root: Path) -> tuple[list[Plugin], list[str]]:
    """plugins.cpp Scan: every enabled mod's plugins\\*.dll, a later mod
    winning a shared name. Returns (plugins, notes on the ones SWSE skips)."""
    found: dict[str, Plugin] = {}
    notes: list[str] = []
    for mod in enabled_mods(mods_root):
        pdir = mod / "plugins"
        if not pdir.is_dir():
            continue
        try:
            files = sorted((f for f in pdir.iterdir() if f.is_file()), key=lambda f: f.name.lower())
        except OSError:
            continue
        for f in files:
            if len(f.name) <= 4 or f.name[-4:].lower() != ".dll":
                continue
            name = f.name[:-4].lower()
            if not valid_switch_name(name):
                notes.append(f"{mod.name}\\plugins\\{f.name}: '{name}' is not a valid switch "
                             "name (a-z 0-9 _ -, up to 31 characters) - SWSE skips it")
                continue
            if reserved(name):
                notes.append(f"{mod.name}\\plugins\\{f.name}: '{name}' is a built-in switch's "
                             "name - SWSE refuses it")
                continue
            if name in found:
                notes.append(f"{found[name].mod}\\plugins\\{f.name}: shadowed by '{mod.name}' "
                             "(later in load order)")
            info = version_info(f) or {}
            found[name] = Plugin(name=name, mod=mod.name, path=f,
                                 description=info.get("FileDescription", ""),
                                 version=info.get("ProductVersion", "") or info.get("FileVersion", ""))
    return list(found.values()), notes


# ---------------------------------------------------------------- Windows bits
def version_info(path: Path) -> dict[str, str] | None:
    """A DLL or exe's version-resource strings (ProductVersion, FileVersion,
    ProductName, InternalName, FileDescription), read without loading it.
    None when it has none, or off Windows."""
    if sys.platform != "win32":
        return None
    import ctypes
    from ctypes import wintypes
    ver = ctypes.WinDLL("version")
    ver.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    ver.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    ver.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    ver.GetFileVersionInfoW.restype = wintypes.BOOL
    ver.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR,
                                   ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
    ver.VerQueryValueW.restype = wintypes.BOOL
    p = str(path)
    size = ver.GetFileVersionInfoSizeW(p, None)
    if not size:
        return None
    buf = ctypes.create_string_buffer(size)
    if not ver.GetFileVersionInfoW(p, 0, size, buf):
        return None
    ptr, n = ctypes.c_void_p(), wintypes.UINT()
    langs = []
    if ver.VerQueryValueW(buf, "\\VarFileInfo\\Translation", ctypes.byref(ptr), ctypes.byref(n)) and n.value >= 4:
        words = ctypes.cast(ptr, ctypes.POINTER(wintypes.WORD))
        langs = [(words[i], words[i + 1]) for i in range(0, n.value // 2, 2)]
    langs += [(0x0409, 0x04B0), (0x0409, 0x04E4)]
    out: dict[str, str] = {}
    for key in ("ProductVersion", "FileVersion", "ProductName", "InternalName", "FileDescription"):
        for lang, cp in langs:
            q = f"\\StringFileInfo\\{lang:04x}{cp:04x}\\{key}"
            if ver.VerQueryValueW(buf, q, ctypes.byref(ptr), ctypes.byref(n)) and n.value:
                out[key] = ctypes.wstring_at(ptr, n.value).rstrip("\0").strip()
                break
    return out or None


def is_swse_dll(info: dict[str, str] | None) -> bool:
    """TOOL_CONTRACT section 1: InternalName SWSE."""
    if not info:
        return False
    return info.get("InternalName", "").upper() == "SWSE" or info.get("ProductName", "").startswith("SWSE")


def is_swse_file(path: Path) -> bool:
    """SWSE's dinput8.dll, any version: 1.1 has a version resource, 1.0.x does
    not but carries the log line every SWSE writes first."""
    if is_swse_dll(version_info(path)):
        return True
    try:
        return b"==== SWSE injected" in Path(path).read_bytes()
    except OSError:
        return False


def running_game_exes() -> list[str] | None:
    """Full paths of every running stranger.exe; an entry is '' when Windows
    will not say where one runs from. None off Windows."""
    if sys.platform != "win32":
        return None
    import ctypes
    from ctypes import wintypes
    psapi = ctypes.WinDLL("psapi")
    k32 = ctypes.WinDLL("kernel32")
    k32.OpenProcess.restype = wintypes.HANDLE
    k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                               wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    arr = (wintypes.DWORD * 4096)()
    got = wintypes.DWORD()
    if not psapi.EnumProcesses(ctypes.byref(arr), ctypes.sizeof(arr), ctypes.byref(got)):
        return []
    out: list[str] = []
    for pid in arr[: got.value // ctypes.sizeof(wintypes.DWORD)]:
        if not pid:
            continue
        h = k32.OpenProcess(0x1000, False, pid)          # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            continue
        try:
            buf = ctypes.create_unicode_buffer(1024)
            n = wintypes.DWORD(1024)
            if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
                if buf.value.lower().endswith("\\stranger.exe"):
                    out.append(buf.value)
        finally:
            k32.CloseHandle(h)
    return out


def game_running(exe: Path | None = None) -> bool:
    """Is stranger.exe running - from `exe` when given (another copy of the
    game does not lock this one's files)."""
    exes = running_game_exes()
    if not exes:
        return False
    if exe is None:
        return True
    want = str(Path(exe)).lower()
    return any(e.lower() == want for e in exes)


# ---------------------------------------------------------------- the mailbox
def mailbox_send(mods_root: Path, lines: list[str], timeout: float = 6.0,
                 poll: float = 0.15) -> str | None:
    """Run console lines in the running game through SWSEMods\\SWSE Console\\
    remote_in.txt (TOOL_CONTRACT.md section 2). The reply's lines, or None if
    none came within `timeout` - the game reads the mailbox only while it
    draws frames, and the request then waits on disk until it does."""
    box = Path(mods_root) / "SWSE Console"
    rin, rout = box / "remote_in.txt", box / "remote_out.txt"
    prev = 0
    try:
        first = rin.read_text(encoding="ascii", errors="replace").split("\n", 1)[0].strip()
        prev = int(first)
    except (OSError, ValueError):
        prev = 0
    seq = prev + 1
    body = f"{seq}\r\n" + "".join(f"{ln}\r\n" for ln in lines)
    box.mkdir(parents=True, exist_ok=True)
    rin.write_bytes(body.encode("ascii", errors="replace"))
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        time.sleep(poll)
        try:
            text = rout.read_text(encoding="latin-1")
        except OSError:
            continue
        rows = text.replace("\r", "").split("\n")
        if not rows or rows[0].strip() != str(seq) or "<<END>>" not in text:
            continue
        reply = "\n".join(rows[1:])
        return reply.split("<<END>>", 1)[0].rstrip()
    return None
