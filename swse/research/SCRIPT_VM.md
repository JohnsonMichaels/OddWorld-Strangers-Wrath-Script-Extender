# Stranger's Wrath - Script VM reverse engineering

The game runs `.foo` scripts (plain-text C-like source stored in the `.smh`
blockmaps) through a native interpreter in `stranger.exe`. Each script verb is
a native C++ handler registered by name.

## What's cracked

- **`foo_api_catalog.md`** - every script function called by the shipping game's
  scripts (149 seen in use), grouped by purpose, with real argument examples.
- **`script_handlers.tsv`** - the FULL registration table: **348 script
  function names → native handler addresses** in stranger.exe, auto-extracted.

### How the table was found
Registration compiles to, per function:
```
push <retTypeStr> ; push <argSigStr> ; push <nameStr> ; push <handlerFn>
mov ecx, esi      ; (script-system registrar `this`)
call Register
```
The name string lives in `.rdata` (~VA 0x77b000-0x77d400, a 548-string pool of
verb names + enum constants). The handler pointer is the `push <imm32>` into
`.text` immediately after the name push. Scanning `68 <rdataVA> 68 <textVA>`
across the image yields the whole map. (image base 0x400000; .text 0x401000,
.rdata 0x750000, .data 0x7e2000.)

### Key handler addresses (image base 0x400000)
| Verb | Handler | Notes |
|---|---|---|
| GiveAmmo | 0x5610F0 | 1 int arg (ammo count; -1 = infinite) |
| GiveAllAmmo | 0x560E30 | |
| GiveCrossbow | 0x561770 | virtual-call through ctx |
| TakeAllAmmo | 0x560B90 | |
| MakePlayerSteef | 0x54D2F0 | form switch |
| MakePlayerStranger | 0x54D390 | |
| SetSteefNaked | 0x54D820 | |
| SetToMaxHealth | 0x5552F0 | |
| SetToMaxHealthAndStamina | 0x555260 | |
| SetHealth | 0x555290 | |
| Kill | 0x555380 | |
| TeleportHome | 0x54E2C0 | virtual-call through ctx |
| GetMoolah / SetMoolah | 0x5630E0 / 0x563140 | |
| GiveArtifact | 0x563560 | |
| GeneralStore_SetItemQuantity | 0x562A50 | |
| StartJobOnSteef | 0x559730 | |

### Calling convention (from prologue disassembly)
Handlers are `__cdecl`, first stack arg is a **ScriptContext\*** the interpreter
supplies; further args are the marshalled script args. Several do
`mov ecx,[esp+arg]; mov eax,[ecx]; call [eax+0x74]` - i.e. they invoke a virtual
method on the context. So they CANNOT be called as plain `f(int)`; the bridge
must supply a valid context.

### Built-in debug cheats (from the name pool - GameButton enum)
`eDebugGiveAllAmmo`, `eDebugInvincible`, `eDebugCycleUpgradeUp/Down` (crossbow
upgrades), `eDebugReload`, `eFlyCam`, `eFreezeCam`, `eToggleGrab`, `eResetPos`,
`eDebugGenericAction`, `eCheatModifier`. These route through the INPUT system,
not the script VM - likely a simpler bridge (inject the GameButton) than
marshalling script args.

## The bridge (next milestone)
Two candidate paths to make SWSE Console commands actually fire:
1. **Capture-and-replay** - inline-hook one handler (e.g. GiveAllAmmo 0x560E30);
   when the game calls it, record the ScriptContext\* + arg layout it passes,
   then reuse that context to call any handler with our own args. Empirical,
   safe, proven method (same approach that cracked the depth buffer).
2. **Debug-button injection** - find the GameButton dispatch and inject
   `eDebugInvincible` / `eDebugGiveAllAmmo` / `eDebugCycleUpgradeUp`. Uses the
   devs' own cheat path; no arg marshalling.

Data-side alternative (works today, no bridge): edit the `.foo` script text in
the `.smh` blockmaps (e.g. flip an ammo grant to `-1`).

## Full function catalog (2026-07-24)

Beyond the 348 script-registered verbs, `ALL_FUNCTIONS.tsv` / `ALL_FUNCTIONS.md`
catalog **every function in stranger.exe** - 12,259 function starts, enumerated
since the binary ships with no symbol table and no debug directory (checked:
neither COFF symbols nor a CodeView/PDB pointer are present - fully stripped).

**Method** (standard disassembler bootstrapping):
1. Every `CALL rel32` (`0xE8`) instruction's target is a confirmed function
   start (9,497 found this way).
2. Every classic `push ebp; mov ebp,esp` prologue (`55 8B EC`) is also treated
   as a start - catches functions only reached via vtable/function-pointer,
   never a direct call (3,362 found).
3. The 335 already-named script handlers are unioned in directly (most of
   them are invoked ONLY through the registration table's indirect calls, so
   they rarely show up as a `CALL` target or a classic prologue - this is
   exactly why cross-referencing found just 18/335 before this union step).
4. Approximate function boundary = next function's start address.
5. For each function, any string referenced in its byte range (`push imm32`
   pointing into `.rdata`/`.data` that resolves to a printable C string) is
   pulled as a naming hint - this codebase embeds a lot of literal file paths
   and debug/assert strings, which is unusually helpful for this.

**Coverage**: 335 named (script VM), 600 with a string hint, 11,324 fully
unlabeled (typical for heuristic-only enumeration without full CFG analysis -
most internal engine functions have no nearby literal string to hint at their
purpose). This is raw material for the script extender to grow into: any
`0x` address in the TSV is callable the same way the 348 script functions are
(RVA off `GetModuleHandleA(NULL)`), once its calling convention and purpose
are worked out - the hints and the 348 known neighbors are the starting
points for that.

## RTTI class / vtable recovery (2026-07-24) - `RTTI_CLASSES.md`

Second identification pass, much bigger payoff: this binary is a normal MSVC
C++ build with RTTI enabled, so every polymorphic class has a recoverable
**vtable** - a list of pointers to its virtual methods. Method:
1. Find each class's `TypeDescriptor` via its mangled RTTI name string
   (`.?AV<ClassName>@@`, 490 found).
2. Find the `RTTICompleteObjectLocator` (COL) that references each
   TypeDescriptor (x86 COL: `{signature=0, offset, cdOffset, pTypeDescriptor,
   pClassHierarchyDescriptor}`, 20 bytes).
3. Find where each COL's own address is referenced - the vtable's method
   array starts immediately after that pointer slot.
4. Read consecutive `.text` pointers as the method list until one doesn't
   land in `.text`.

**Result: 432 classes, 548 vtables, 10,560 virtual methods** now attributed to
a specific class (labelled `ClassName::vfunc<N>` - merged into
`ALL_FUNCTIONS.tsv`'s name column). Combined with the 335 script functions,
**663/12,259 functions are now named or class-attributed.**

Classes directly relevant to tonight's investigations:
- **`NPCJump`** (43 methods) - a dedicated jump class; strong candidate for
  where the double-jump-counter logic Cheat Engine found (`+0x1E8`) actually
  lives.
- **`FlyCamera`** (29 methods) - shares the exact same 29-method vtable shape
  as `FollowCamera`, `FixedCamera`, `GameplayCamera`, `OrbitCamera`,
  `CinematicCamera`. Since they implement the same interface, comparing slot
  N across the working camera types against FlyCamera's slot N is a real path
  to understanding (and eventually driving) it - much stronger than the
  dead-end GameButton input search from earlier.
- **`PlayerImpl`** (195 methods, the largest recovered vtable) - almost
  certainly the core player behavior class.

**Next step for any of these**: vtable/class membership tells us *which*
class a function belongs to, not *what it does* - that needs actual
disassembly reading. Ghidra (free) auto-decompiles a function into C-like
pseudocode, which is far more tractable to read than raw hex for figuring out
a specific method's purpose. Recommended flow: pick a method address from
`RTTI_CLASSES.md`, load it in Ghidra, paste the decompiled pseudocode back
for review/naming, then confirm live via `call 0xADDRESS` in the console.
