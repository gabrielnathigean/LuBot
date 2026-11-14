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
