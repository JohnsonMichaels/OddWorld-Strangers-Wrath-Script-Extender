# SWSE - Stranger's Wrath HD `.smb` Format Notes

Status: reverse-engineering in progress. Everything below verified empirically against
all 1,222 `.smb` archives in the Steam HD release (2026-07).

> **STATUS 2026-09-28: written 2026-07-23; corrections are marked where they apply.** The container
> layout below is implemented in `oddforge/container.py` (parse and byte-identical rebuild,
> 1,222/1,222, checked by a round-trip script kept in the development repository). `oddforge/toc.py` decodes the TOC node header and the
> texture records, and `oddforge/records.py` finds and edits generic records. Later format work:
> ODDVIEW.md (character bundles, `.geo` records) and swse/research/AT3_DISCOVERIES.md (record
> framing, prefs records, `.smh`, `.lvl`).

## Container layout (VALIDATED on 1222/1222 archives)

All values little-endian. Strings are `uint32 length` + exactly that many ASCII bytes
(no null terminator, no alignment/padding - fields are byte-packed).

```
uint32  magic            = 0x3A4B5C6D  (universal)
uint32  version          = 5           (universal)
string  self_path        e.g. "\data\bundles\region_00\lm_level_00\npc_2.smb"
uint32  A  header_block_size    file offset where section1 begins (e.g. 0x800)
uint32  B  toc_used_bytes       bytes of A actually used by the TOC (rest is padding)
uint32  C  section1_size        allocated size; may be 0
uint32  D  section2_size        allocated size; may be 0
uint32  E  section1_used?      often slightly < C
uint32  F  section2_used?      often slightly < D
uint32  G  entry_count          number of TOC entries (matches file count, e.g. 2 textures)
uint32  H  section_count?       2 for 1214 archives, 1 for 8 archives
uint32  sentinel         = 0xBEEF1234  (universal - appears at this fixed position)
...     TOC entries follow (per-entry layout NOT yet decoded - see below)
...     padding to offset A
[A .. A+C)      section 1 data
[A+C .. A+C+D)  section 2 data
```

**Invariant (validated, zero failures): `file_size == A + C + D`.**

## TOC entry region (partially decoded, npc_2.smb reference)

After the sentinel: `0x4DFAA77E` (Unix time → June 2011, HD build timestamp - repeats
per entry), unknown ids/hashes (`0xE3C20CDE` repeated), then per entry:

- entry name as length-prefixed string (e.g. `\data\textures\attachments\hat_GI.tga`)
- ~97 bytes of per-entry record: contains what look like dimensions (128, 128),
  a mip/format field (5), size-like values (0x12F8), float 1.0, and the build
  timestamp again. **Fields are NOT 4-byte aligned** - parse sequentially only.

> **NOTE 2026-09-28:** decoded since, in part. Each TOC node has a 40-byte header (build timestamp
> `0x4DFAA77E`, node id, data offset into section 1, the id repeated), then the name. A texture
> record has a `0x29` marker at +12, the format at +21 (12 = DXT1), then width and height at
> +25 / +29 (`oddforge/toc.py`). Records of every kind, prefs included, can be found and exported
> (`oddforge/records.py`). A prefs or tag record describes itself, `[0x000B4265][class hash]
> [params...]`, in the order of the class's ParamIO descriptors (AT3_DISCOVERIES.md §0).

## TOC region - it's an object-graph serialization, not a flat file table

Findings from npc_10.smb (2026-07-23):

1. **NPC behavior scripts are embedded in NPC archives.** npc_10 contains plain-text
   `.foo` source: `variable bool ImToast = false; OnDeath(){ Set("TUT_giveAmmos",false);
   SetJournalText(0, "journal_blisterzBountied"); ... } OnBounty(){ ... }` - per-NPC
   event handlers (`OnDeath()`, `OnBounty()`, `{startup}()`), each preceded by a
   length-prefixed function-name string and hash. NPC behavior modding is therefore
   per-archive and text-based.

2. **The engine uses reflective serialization.** Entries contain embedded type
   metadata: `class CClassDef<class Vec3>`, field names like `m_min` / `m_max`
   followed by float vector data. The format is partially self-describing - great
   news for a repacker.

3. **Entries are typed nodes, not uniform records.** The bytes between entry names
   vary by type: texture entries carry dims/mips/format (e.g. 128,128,5), `.geo`
   entries carry bounding-box floats + class metadata, script entries carry source
   text with `{EOF}` markers. Repeated 4-byte hash-like ids (e.g. `0x95FBB271`,
   `0x2DFD1072`) appear to link nodes together. Same name can appear multiple times
   (e.g. `webbing_color.bmp` twice) - likely reference vs definition nodes.

Next: parse the TOC as a node stream (hash ids + length-prefixed strings + typed
payloads) rather than a name/record table.

## Other formats in the game

| File | Notes |
|---|---|
| `*_blockmap.smh` | DIFFERENT format, magic `0xBEEF2B16`. Contains **plain-text `.foo` script source** (C-like: `OnEnter(Object obj){...}`, `GetHealth()`, `Set()`, `StartScript()`), with `{EOF}` markers. Level logic/events/NPC triggers live here. 10 files (one per region + utility). |
| `*_blockmap.txt` | Plain-text level manifest: lists the level's npc/zone/cine SMBs and script paths. |
| `*.lvl`, `*.sbl` | Not yet examined. |
| `data\audio\*.fsb` | Standard FMOD sound banks - existing tools handle these. |
| `bin\cg.dll` | NVIDIA Cg shader pipeline (renderer is shader-based → ReShade yes, RTX Remix no). |

> **NOTE 2026-09-28:** two rows above moved on.
> - `.lvl` is now readable and writable with a schema read from the exe's reflection (`tools/lvl_schema.json`,
>   development repository only), which parses all 9 shipped level roots; AT3 decoded the object-record chain
>   (token `0x7A60600D`, class-hash marker `0x000B4265`, zone, rotation, translation, scale; CHANGELOG.md 1.1).
> - `.smh`: AT3 found that the blockmap holds the section-1 copy the game actually loads, indexed
>   by cumulative cursors and ended by `0xCAFED00D` (AT3_DISCOVERIES.md H2). Script extraction
>   through it is still not built in oddforge.

## Archive inventory

- `data\global\*.smb` - 72 archives: `global_player.smb`, `global_stranger.smb`,
  `global_appglobal.smb`, GUI screens, loading screens, `global_critterpool.smb`.
- `data\bundles\region_XX\lm_level_XX\` - per level: `npc_N.smb` (one per NPC type,
  models+textures), `zonebundle_N.smb` (world geometry), `cine_N.smb` (cutscenes,
  Granny `.gr2` animation refs), `lm_level_XX_tgl.smb` (large, contains `.foo` refs).

> **CORRECTED 2026-09-28 (swse/research/PLAYNPC.md, "What a character needs"):** `npc_N.smb`
> blocks are not one per NPC type. A character's body is one npc block, and its animations are in
> one to three npc blocks that other characters often share (in lm_level_02, one block holds 45
> townsfolk animations used by every townsfolk variant). A character's prefs and animation config
> live in `lm_level_XX_tgl.smb`, which also holds the level's character type table (ODDVIEW.md).

## Next steps

1. Decode the per-entry TOC record: find data offset/size fields (candidates exist),
   entry type ids, texture format enum.
2. Extractor: `bounty catch <archive.smb>` → dump named files.
3. Byte-identical rebuild: `bounty cashin` → repack; diff against original. When a
   rebuilt archive is byte-identical, we understand every field.
4. `.smh` blockmap format (magic 0xBEEF2B16) → script extraction/injection.

> **STATUS 2026-09-28:** 3 is done: `oddforge.container.SmbContainer.parse(...).build()` rebuilds
> all 1,222 archives byte for byte (a round-trip script in the development repository checks it), and modified rebuilds load
> in game. The `bounty catch` / `bounty cashin` commands were never built under those names: texture
> export is `python -m oddforge.dump <game>\data <out>`, and mods are applied and reverted by
> `oddforge/modloader.py`, through the Mod Loader GUI (`studio.py`). A command-line front end, `tools/mods_cli.py`, is in the development repository only.
> 1 is done in part (see the notes above); 4 is not started in oddforge.
