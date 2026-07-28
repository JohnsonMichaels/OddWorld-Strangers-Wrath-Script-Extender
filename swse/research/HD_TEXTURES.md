# HD texture pipeline - state, and the open bug

## The pipeline

Three steps, with the AI upscaling done outside by a tool the user runs:

```
tools/hd_export.py  ->  PNGs  ->  [ Upscayl / ESRGAN, 2x ]  ->  tools/hd_pack.py
```

Archives are **never modified**. SWSE substitutes textures at GPU-upload time
(`glCompressedTexImage2D`), so a bad texture can never corrupt a save or break
the level loader - worst case it looks wrong and you delete the `.oft`.

**The trick that makes bulk work:** every PNG is named after its runtime
fingerprint (`8877146C.png`), which is the key SWSE matches at upload. Upscalers
preserve filenames, so packing needs no lookup table and duplicates across
archives collapse for free (54 collapsed in one small batch).

## Which model, by content type

Measured by A/B on the same textures, not assumed:

| Content | Model | Why |
|---|---|---|
| Grain/material - critters, metal, dirt, rock | **UltraSharp** | preserves surface grain |
| Flat graphic art - crates, decals, signs, UI | **upscayl-standard** | smoother curves |

`upscayl-standard` treats grain as noise and removes it: the crossbow's worn
metal came out flat plastic. UltraSharp keeps it. The reverse holds on flat
stencil art, where UltraSharp faithfully preserves the original's pixel
stair-stepping while standard smooths it into clean curves.

Packing is per-fingerprint, so the two can be mixed file by file.

## Hold these back from AI upscaling

- **Gloss maps** (`*_Gloss`) - specular data, not colour. A photo model
  hallucinates detail into what should be smooth gradients and shifts how shiny
  the surface reads.
- **Cube map faces** (`*CubeFace*`) - upscaling faces independently breaks the
  seams between them.
- **Normal maps** - need upscale-then-renormalise, or lighting goes wrong.

`hd_export.py` does not separate these; the split is done afterwards (see the
`special/` subfolders).

## Tiling terrain: check before packing

`tools/seam_check.py` measures whether a texture still wraps seamlessly, by
comparing the edge-to-edge difference against the interior difference. Of 108
ground textures, **56 tile seamlessly** - for those, a broken wrap puts a grid
of seams across the entire landscape.

Run it with `--compare <upscaled>` before packing any terrain: it reports which
textures tiled before and no longer do, by name.

## Alpha: checked and ruled out

`tools/alpha_check.py` inspects the original DXT1 blocks directly - a block uses
transparency when `color0 <= color1` and index 3 appears. Of 89 installed
textures, **zero** use 1-bit alpha, so dropping alpha (which the codec does:
`encode_dxt1` calls `convert("RGB")`) is not currently causing any artifact.

It would matter the moment decals or foliage cutouts are packed. The tool exists
to catch that.

## OPEN BUG: non-square textures render black

Symptom: entire building facades render solid black in town - the chicken
structures specifically.

All 7 non-square textures in the packed set are the prime suspects:

```
Chicken_RV_temp      512x1024      chicken_goods_01    256x64
chickenTrailer       512x256       clothes_chicken     128x64
reload_arm_color     512x256       twnsflk_SwrWorkrHat 256x128
ThirdPersonXbow       64x128
```

**Likely cause:** `hd_pack.py` builds its mip chain down to "min dimension 8".
On a square 512x512 that gives the expected count. On a 256x64 the short side
reaches 8 after 4 levels while the long side is still 32 - so the level count
differs from what the engine computed for the vanilla texture. The engine
derives data size from dimensions and mip count (no stored size field - see
`resize.py`, verified across 413 textures), so a mismatch means it reads the
wrong amount of data.

`resize.py` already documents the engine's rule: `mips = log2(min(w,h)) - 2`,
and notes that a wrong mip count **crashes**. `hd_pack.py` does not follow it.

**Not yet confirmed.** The clean test is to disable the HD textures, warp to
force a reload, and see whether the black goes away. Everything square has
looked correct all along, which is consistent with the theory.

**Current state:** 82 square textures enabled, the 7 non-square ones moved to
`SWSEMods\SWSE HD\textures_DISABLED`.

## Coverage gap

`hd_export.py` only handles DXT1. A character-wide export skipped **417
non-DXT1 textures** - roughly three-quarters of the character art. Adding
DXT3/DXT5 support is the single biggest expansion available to this pipeline,
far more valuable than further model tuning.
