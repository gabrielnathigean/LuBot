# Moonland protocol RE — working notes

Status: **paused, to be continued**. This documents everything confirmed so far
about why `ciws/L2Bot` does not work against Moonland's `L2.bin`
("Custom Interlude Remastered"), the tool built to dig further, and the raw
data collected. Nothing here is wired into the bot yet.

## TL;DR

- The bot's Interlude hooks fail before `Application::Start()` finishes:
  `VersionAbstractFactory::GetFactory` builds `NetworkHandlerWrapper`, whose
  `Init()` calls `GetProcAddress(hEngine, "?Tick@UNetworkHandler@@UAEXM@Z")`
  and gets `NULL` — **the whole `UNetworkHandler` class is gone** from this
  build's `Engine.dll` (2023), not just renamed. Confirmed twice: statically
  (0/13 `UNetworkHandler` symbols in the export table) and dynamically
  (`app.log`: `Unable to initialize the injected runtime`, immediately).
- The real packet-receive path still exists, just moved to an unexported
  virtual method. Found it, hooked it, and captured **live decrypted packets**
  from the actual Moonland server.
- The wire-protocol opcodes were also renumbered — Interlude's
  `SYSTEM_MESSAGE = 0x64` never appears in 576 captured packets. This is not
  a matter of relinking the old handlers to new names: Moonland's opcode
  table has to be rebuilt from scratch, packet by packet.

## How the dispatcher was found

1. `L2BotDll/Common/NetworkProbe.{h,cpp}` IAT-hooks `Engine.dll`'s imports of
   `recv`/`WSARecv` (turned out this build only uses plain blocking `recv` —
   `WSARecv` isn't even imported).
2. Every `recv()` call during a live session came from the same address,
   `Engine.dll+0x303a18` — a generic Unreal socket `Tick` that frames
   messages by a 2-byte length prefix, then (once a full message is
   assembled) does `mov ecx, esi; call dword ptr [eax+0x7c]` — a virtual
   call through the connection object's vtable, slot `0x7c`.
3. That connection pointer is recovered from `ESI` with a `__declspec(naked)`
   trampoline in front of the `recv` hook: `ESI` is callee-saved across
   `recv()` by the x86 calling convention, so the `Tick` function's `this`
   survives into the hook untouched, with no debugger needed.
4. `vtable[0x7c / 4]` is `Engine.dll+0x302820`: confirmed by static disasm to
   decrypt the buffer in place, then read the real opcode from `buffer[2]`
   (`buffer[0..1]` is still the 2-byte length prefix) and build a
   `{opcode, subOpcode, dataPtr, length}` descriptor — functionally the old
   `UNetworkHandler::AddNetworkQueue`, just never exported by name here.
5. `NetworkProbe::Install()` patches `vtable+0x7c` directly (`VirtualProtect`
   + pointer swap, original forwarded first, every time) and logs every
   decrypted packet to `netprobe.log` next to `app.log`.

This proves live decrypted packets are reachable and pins down exactly
where. **It does not hook anything the bot core relies on** — it is a
read-only probe, separate from `NetworkHandlerWrapper`/`GameEngineWrapper`.

## Confirmed exported-symbol gaps (static, from `Engine.dll`)

| Expected (old `ciws/L2Bot` Interlude hooks) | Present in Moonland's `Engine.dll`? |
|---|---|
| 13 `UNetworkHandler::*` symbols (`Tick`, `AddNetworkQueue`, `GetNextItem`, …) | **0 / 13** |
| 13 `UGameEngine::On*`/`Tick`/`AddAbnormalStatus`/etc. | **1 / 13** (only `GetMaxTickRate`) |
| `UGameEngine` exports overall | 81, none starting `J`–`Z` — i.e. no `On*`, `Tick`, `Say*` at all |

## Captured opcode frequency (576 packets, ~46s, mixed idle/walk/attack)

| opcode | count | len seen | working hypothesis (unconfirmed) |
|---|---|---|---|
| `0x2f` | 333 | 31 | frequent position sync (two coord triples per packet — self + target?) |
| `0x27` | 315 | 11 fixed | very high frequency, tiny payload — per-tick/heartbeat signal |
| `0x79` | 288 | 24 | position sync variant, one coord triple + trailing dword |
| `0xce` | 156 | 18 fixed | byte-identical across repeats while nothing changed — a state/flag packet |
| `0x31` | 101 | 384 | **UserInfo/CharInfo-like**: contains a UTF-16LE name string (`"RedArrow"` seen) and large stat block |
| `0x48` | 74 | 71 | **MoveToLocation-like**: object id + two (X,Y,Z) coordinate triples |
| `0x54` | 73 | 27 | object id + short payload, co-occurs with `0x48` in the same burst |
| `0x62` | 27 | — | not yet sampled |
| `0x08` | 23 | — | not yet sampled |
| `0x24` | 22 | — | not yet sampled |
| `0x72` | 20 | — | not yet sampled |
| `0x4a` | 19 | — | not yet sampled |
| `0x23` | 12 | — | not yet sampled |
| `0xfe` | 11 | — | classic Interlude "extended opcode" marker byte — may still mean the same thing here |
| `0x18` | 6 | — | not yet sampled |
| rest | ≤3 each | — | one-off / rare events, not yet sampled |

None of this is confirmed against actual on-screen events yet (no isolated,
single-action capture has been read back against a specific opcode). The
hypotheses above are pattern-matching on payload shape (coordinate-sized
dwords, embedded UTF-16 text, repeated object-id-looking values) only.

### Sample: `0x31` (len 384) — probable UserInfo/CharInfo

```
80 01 31 00 e8 2f 03 00 38 33 03 00 20 06 00 00 00 00 00 00 18 b6 13 10
52 00 65 00 64 00 41 00 72 00 72 00 6f 00 77 00 00 00 ...   <- "RedArrow" UTF-16LE
```
`e8 2f 03 00` / `38 33 03 00` / `20 06 00 00` read as little-endian dwords:
`0x00032fe8`, `0x00033338`, `0x00000620` — magnitudes consistent with L2
world coordinates (X, Y, Z). `18 b6 13 10` looks like the same
pointer-shaped "object id" seen in `0x48`/`0x54` samples below.

### Sample: `0x48` (len 71) — probable MoveToLocation broadcast

```
47 00 48 00 00 00 00 c0 36 5d 10 c0 36 5d 10 2f 23 00 00 01 00 00 00 ...
...28 2f 03 00 58 37 03 00 28 06 00 00 00 00 00 00
   28 2f 03 00 58 37 03 00 28 06 00 00 00 00 00 00
```
`c0 36 5d 10` repeats twice right after the header (object id?), followed by
two back-to-back identical (X,Y,Z) triples later in the payload — matches
the shape of "current position, destination position" in a move packet when
the object has already arrived (both triples equal).

### Sample: `0x27` (len 11, fixed) — unexplained high-frequency packet

```
0b 00 27 23 2d 10 10 02 00 00 00
0b 00 27 88 21 10 10 02 00 00 00
```
Only 4 payload bytes after the opcode vary between samples (`23 2d 10 10` vs
`88 21 10 10`); the trailing `02 00 00 00` is constant across every capture
so far. 315 occurrences in 46 seconds (roughly one every 150 ms) — too
frequent for a player action, more likely a per-NPC or per-tick server
broadcast.

## Confirmed opcodes (session 2 — single-target lifecycle correlation)

Method that worked: pick one object id from an `Attack` burst, `grep` the
whole log for that exact byte sequence (space-separated hex, e.g.
`grep -E "f2 7d 4e 10"`), and read its entire lifecycle in order. Far more
reliable than frequency counting, which is dominated by ambient NPC traffic
in any populated zone.

| opcode | len | confirmed meaning | evidence |
|---|---|---|---|
| `0x0` | 52 | **Die** | fires at the exact timestamp the target's HP bar/model disappeared; object id + 48 zero bytes |
| `0x33` | 53 | **Attack** | `attackerId, targetId, 00000000, damage(dword), hitFlag(dword), 00000005, attackerPos, targetPos`; attacker id constant across every sample = our own character |
| `0xc` | ~200-230 | **NpcInfo** | contains the NPC's name as UTF-16LE, readable (`"Arcana Treasure Chest"`, `"Mechanic Worker"`, `"Drainpipe Phantom"`, …) |
| `0xc1` | 8 | per-object **state byte** | same object: `04` while alive/fighting → `05` at the instant of death |
| `0x8` | 11 | **despawn / remove object** | fires ~9s after `0x0` for the same id — matches L2's usual corpse decay delay |
| `0x23` | 27 | probable **attack start / engage** | fires once, only at the start of a fight on a given target, never repeats like `0x33` does |

Still open on the same capture: `0x79`/`0x47` (fire in lockstep, look
position-related but their trailing dword doesn't move monotonically — not
yet understood), `0x25`/`0x4b`/`0x26` (fire once each near combat start or
death, meaning unclear), `0x62` (**previous HP-bar hypothesis retracted** —
its header shape isn't consistent across samples, offsets used earlier were
wrong).

## `Application::Init()` now completes (session 3)

`trySplice()` (in `Common/apihook.{h,cpp}`) is `splice()` without the throw
on a missing export — it returns `nullptr` and lets the caller keep going.
Every `*Wrapper::Init()` in `Versions/Interlude/GameStructs/` now uses it.
Verified with a temporary per-step log line (removed once confirmed):
`Application::Init()` runs `NetworkHandler.Init()` → `GameEngine.Init()` →
`L2GameData.Init()` → `FName.Init()` end to end on Moonland's `Engine.dll`,
where it used to throw on the very first missing export and skip
`WorldHandler::Start()` outright.

**This does not make anything show up in the bot's UI yet.**
`NPCRepository::GetEntities()` (and `HeroRepository`'s equivalent) do not
read packets at all — they call `NetworkHandlerWrapper::GetNextCreature()`,
which reads the live object list directly out of the game's own memory
through the same `UNetworkHandler` export that is missing here. With that
pointer null, `GetEntities()` always returns empty, independent of whether
`AttackedEvent`/`CreatureDiedEvent` fire correctly.

Two separate things are needed before combat shows up anywhere real:

1. **Entity source**: either find the modern replacement for
   `UNetworkHandler::GetNextCreature`'s live object enumeration (a different,
   harder RE problem — likely walking an `FObjectArray`/level actor list
   directly), or build a parallel `EntityRepositoryInterface` implementation
   that keeps its own `{objectId → snapshot}` map built purely from observed
   packets (`0xc` NpcInfo for creation/name, `0x33`/`0x0` for combat/death,
   the still-unidentified position opcodes for movement). The second option
   reuses everything already confirmed in this document and needs no further
   live-memory RE.
2. **Event wiring**: `NetworkProbe`'s `vtable+0x7c` hook is currently
   read-only (logs and forwards, dispatches nothing). It would need to parse
   `0x33`/`0x0`/`0xc` for real and call
   `Services::ServiceLocator::GetInstance().GetEventDispatcher()->Dispatch(...)`
   with `AttackedEvent`/`CreatureDiedEvent`, feeding exactly the
   `NPCRepository` subscriptions that already exist and now actually run.

Neither is done. This is the next real increment, and on its own is enough
work to be its own session.

## Tooling

- `L2BotDll/Common/NetworkProbe.h` / `.cpp` — the probe itself (IAT hook +
  vtable hook), compiled into `L2BotDll.dll`, active from `RuntimeThread`
  before `Application::Start()` runs. Entirely diagnostic: forwards every
  call to the original function first, never skips or delays anything,
  wrapped in SEH so a failure degrades to "did nothing" rather than crashing
  the host process.
- Output: `netprobe.log`, next to `app.log`, in the same directory as
  `L2BotDll.dll`.
- Static disassembly helper scripts (`pe_report.py`, `disasm_caller.py`,
  `check_exports.py`, `sweep.py`) live outside this repo, in the session's
  scratch directory — see [`RE_L2BIN.md`](../../../RE_L2BIN.md) in the
  `DUMPL2` workspace root for the original static analysis of `L2.bin`
  itself (packer identification, import table, entry point) that started
  this investigation.

## What "continuing" looks like

1. Restart `L2.bin` + `Client.exe` fully between each capture (Windows keeps
   an already-loaded DLL's old in-memory code even after the file on disk is
   rebuilt — several captures in this session were accidentally taken
   against a stale in-process build until the game was fully closed).
2. One isolated action per capture: stand still only, walk once only, attack
   once only, take damage once, die once, use one skill once. Read
   `netprobe.log` immediately after each and correlate by timestamp against
   what happened on screen in that exact window.
3. Cross-reference candidate `SystemMessage`-shaped packets (small-ish,
   containing short integers) against Moonland's own `.dat`/string tables in
   `Client/` for message IDs matching visible chat text (e.g. "That is an
   incorrect target", "2,000 MP has been restored") — this gives an
   unambiguous anchor instead of guessing from payload shape alone.
4. Once a handful of opcodes are nailed down with certainty (position,
   attack, HP/MP, death are the minimum the bot's existing `WorldHandler`/
   `IncomingMessageProcessor` needs), replace `NetworkHandlerWrapper`'s
   `AddNetworkQueue` hook with a hook on `vtable+0x7c` using the real opcode
   values, and start wiring `L2ParamStack` parsing against the new payload
   layouts (they will not match Interlude's old field order even where the
   opcode number coincidentally matches).

This is a multi-session reverse-engineering project on its own, not a
follow-up patch.
