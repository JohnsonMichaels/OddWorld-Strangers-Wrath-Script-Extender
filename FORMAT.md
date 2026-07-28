# SWSE - Stranger's Wrath HD `.smb` Format Notes

Status: reverse-engineering in progress. Everything below verified empirically against
all 1,222 `.smb` archives in the Steam HD release (2026-07).

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

## Archive inventory

- `data\global\*.smb` - 72 archives: `global_player.smb`, `global_stranger.smb`,
  `global_appglobal.smb`, GUI screens, loading screens, `global_critterpool.smb`.
- `data\bundles\region_XX\lm_level_XX\` - per level: `npc_N.smb` (one per NPC type,
  models+textures), `zonebundle_N.smb` (world geometry), `cine_N.smb` (cutscenes,
  Granny `.gr2` animation refs), `lm_level_XX_tgl.smb` (large, contains `.foo` refs).

## Next steps

1. Decode the per-entry TOC record: find data offset/size fields (candidates exist),
   entry type ids, texture format enum.
2. Extractor: `bounty catch <archive.smb>` → dump named files.
3. Byte-identical rebuild: `bounty cashin` → repack; diff against original. When a
   rebuilt archive is byte-identical, we understand every field.
4. `.smh` blockmap format (magic 0xBEEF2B16) → script extraction/injection.
