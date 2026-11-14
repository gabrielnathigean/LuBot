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

## Packet-derived entity source (session 4): wired, but hero self-ID is unreliable

`Versions/Moonland/` is a new, parallel `VersionAbstractFactory` (reached via
`Version::classic`, which `dllmain.cpp` now uses unconditionally for this
fork) that reads nothing from live game memory:

- `PacketHandler` registers with `NetworkProbe::SetPacketObserver()` (added
  this session - a plain callback NetworkProbe invokes after forwarding and
  logging, so there is still exactly one owner of the `vtable+0x7c` hook)
  and parses `0x0c`/`0x33`/`0x00` for real: a `{objectId → name, alive}` NPC
  map, and real `AttackedEvent`/`CreatureDiedEvent` dispatch through the
  same `ServiceLocator` event bus `NPCRepository` (the Interlude one) already
  subscribes to.
- `NPCRepository`, `HeroRepository` read that map/state into real
  `Entities::NPC`/`Entities::Hero` objects. `EmptyRepository` and
  `NoopInterfaces.h` satisfy every other slot `VersionAbstractFactory`
  requires (drop/player/skill/item/abnormalEffect/chat, and the
  NetworkHandler/GameEngine/L2GameData/FName hook interfaces) with
  deliberate no-ops - none of those have a confirmed opcode or hook yet.

**Confirmed working, end to end, with temporary diagnostics since removed:**
`Application::Init()` completes, both named pipes connect, the C# client's
`Invalidate()` round-trip fires, and `0x0c` packets do get parsed into the
NPC map in real time (watched live packets turn into map entries).

**Confirmed broken: hero self-identification.** There is still no opcode
for our own CharInfo/UserInfo (see the open list above), so `PacketHandler`
guesses the hero's object id as "whichever attacker id appears most often in
`0x33` packets" (`PacketHandler::GetHeroId()`). This works when the capture
is a single player fighting a single isolated target (confirmed in session
2's lifecycle trace) - it visibly fails in a populated area: one capture
through a busy zone had thirteen `0x33` packets *targeting* the same id
while the *attacker* field was scattered almost evenly across six different
ids, meaning some nearby hostile NPC was attacking several different
players and none of those IDs is the hero's. Caught by hand, not by the
code - the heuristic has no sanity check against this, so it can silently
latch onto the wrong id. This is almost certainly what the "a creature
appeared then disappeared in the UI" symptom was: the inferred hero id
changed briefly to something plausible-looking, then the OutgoingMessageBuilder's
hash diff sent a delete once hit counts shifted again.

**Fixed (session 5): read it from memory, not from a packet.**

Captured a fresh EnterWorld burst (full relog, `Client.exe` hooked before
clicking into the character) and grepped it for `"Dayneve"` across every
opcode, not just `0x0c`. Found it in a new opcode, `0x0b` (len 205,
UTF-16LE name as the very first field, no object id before it) - but a
byte-by-byte scan of that whole packet turned up **zero** bytes with value
`0x10`, anywhere. Every other object id seen anywhere in this protocol
(`0x0c`, `0x33`, `0x00`, the lot) has its high byte fixed at `0x10` - that
matches `L2.bin`'s own fixed, ASLR-off image base (`0x10900000`, see
[`RE_L2BIN.md`](../../../RE_L2BIN.md)), i.e. these "object ids" are raw
client-side pointers serialized straight into the wire protocol, not
abstract server-assigned integers. `0x0b`'s total absence of any such byte
means it doesn't carry a self-reference in that form at all - our own
object pointer is implicit to the client (it already holds it locally) and
is never sent back to us over the network.

So the id has to come from the client's own memory instead:

- `?GEngine@@3PAVUEngine@@A` (`Engine.dll`) - a global variable holding the
  current `UEngine*`/`UGameEngine*`.
- `?GetMyPawn@UGameEngine@@QAEPAVAPawn@@XZ` - exported, non-virtual, takes
  that engine pointer as `this`, returns our `APawn*`. Confirmed in the
  very first static export sweep of this `Engine.dll` (see the dispatcher
  section above) - it was the *one* `UGameEngine` method out of 81 that
  also exists in the old Interlude export list, which is exactly why it
  survived unlike `Tick`/`OnAttack`/etc.
- The returned `APawn*` itself is **not** the network id (it printed as
  `0x16237e00` in testing - Engine.dll/Core.dll heap, no `0x10` high byte).
  A bounded, SEH-guarded scan of the first `0x600` bytes of that struct
  found exactly one dword with a `0x10xxxxxx` shape, at a fixed offset:
  **`+0x58`**. Logged stable across three separate reads in one session,
  and the exact same value matched a live combat packet's target id in
  that same session.

`Moonland::PacketHandler::GetHeroId()` now resolves
`GEngine -> GetMyPawn() -> [+0x58]` directly (re-read every ~20 packets,
guarded by `__try`/`__except`, sanity-checked against the `0x10xxxxxx`
shape on every read so a bad read degrades to "unknown" rather than
silently returning garbage) instead of guessing from attack frequency.
**Confirmed working end to end**: the C# client's Environment tab started
listing real NPC entries (including one correctly flagged `(dead)` after a
`CreatureDiedEvent`) for the first time this session, with `app.log`
staying empty (no errors) throughout.

Two things this did *not* give us yet as of writing that paragraph: our
own name/stats, and position for anyone/anything that hasn't fought
recently.

## Position (session 6): reused the Attack packet, no new RE needed

`0x33` (Attack) turned out to already carry both sides' position, as two
raw signed-int32 `(X,Y,Z)` triples right after the damage/flag/type
fields - confirmed by the packet's own fixed 53-byte length matching
exactly when the layout is: `attackerId, targetId, 00000000, damage,
hitFlag, 00000005, attackerPos, 2-byte gap, targetPos`. `PacketHandler`
now updates `m_HeroPosition` (when the hero is the attacker or the target)
and each `TrackedNpc`'s own `position` the same way, on every `0x33` it
sees. `NPCRepository`/`HeroRepository` feed that straight into
`ValueObjects::Transform` instead of a `(0,0,0)` placeholder.

**Confirmed working, visually, this session**: a plain melee attack
produced a green hero marker on the real map texture, and the attacked
NPC's row showed `distance: 36` - correct for melee range - while every
*other*, never-fought NPC still showed the enormous distance a stale
`(0,0,0)` produces. That's expected, not a bug: position is only as fresh
as an entity's last recorded hit.

**One real gotcha found along the way**: a caster's skill-attack produced
real damage and no `0x33` packets at all in that session - `0x33` only
fires for a plain physical attack. Skill/magic damage uses a different,
still-unidentified opcode (likely among `0x18`, which showed up tied to
the hero's own id at high frequency in that same skill-casting session,
though its field shape didn't look like a position triple on inspection -
not investigated further yet).

## HP/CP delta (session 7): `0x62`, confirmed; max HP is a dead end over the network

`0x62`'s trailing 12 bytes are `(targetId, attackerId, delta)` regardless
of which of its two observed header shapes shows up - confirmed by five
exact matches in one capture, where `delta` was always exactly `-damage`
from the `0x33` that preceded it (`damage=0xd6` → `delta=0xffffff2a`
= `-0xd6`, and so on). `PacketHandler` now keeps a running
`damageTaken` counter per tracked NPC and for the hero.

**Max HP is not obtainable over the network at all, at least not this
way.** The server's native "monster info" window (this build's own
custom feature, opened with Shift+2-click, showing name/max HP/current
HP/aggro/respawn) produces **zero new packets** when opened - it reads
data the client already has locally, i.e. the game's own `.dat` tables on
disk, not anything sent over the wire. That's a file-format RE task,
entirely separate from the protocol work in this document, and not
started. Until then, `damageTaken` is a relative counter, not deliberately
wired into `VitalStats` (hp/maxHp) since there is no real max to make a
percentage meaningful.

## Real target HP (session 8): `0x18`, confirmed against the game's own UI

The user noticed something session 7 missed: targeting a monster shows an
exact `current/max` HP readout (not just a bar) in its own status window -
a different display from the Shift+2-click "monster info" dialog, and
this one *does* update live as the target takes damage, which means it
has to come from the network after all.

Found it: `0x18`, carrying `targetId` then either `(current, max)` the
first time that id is targeted (a byte at offset 12 reads `0x02`) or just
a refreshed `current` on every later packet for the same id (that byte
reads `0x01` instead, and the packet is 5 bytes shorter). Confirmed two
ways:

- The very first `0x18` for a freshly-targeted "Hot Springs Buffalo" read
  `max = 8170` - the *exact* value the Shift+2-click dialog showed for
  that monster earlier in this document, nothing approximate.
- Watching the same id's `current` value while the hero fought it matched
  the live, fluctuating pattern on screen: dropping by the attack's
  damage, then climbing back a fixed ~42-43 between hits from the
  monster's own HP regen - not a simple monotonic countdown.

`PacketHandler` now keeps real `currentHp`/`maxHp` per tracked id (falling
back to the session-7 damage-taken counter, and then to the simple
alive/dead placeholder, for anything never personally targeted);
`NPCRepository` feeds that straight into `VitalStats` as a real
percentage instead of a relative counter.

**A loose end spotted along the way, not investigated**: `0x18` also
fires tied to the *hero's own* id, in a different, longer shape (two
values climbing together by a matching small amount each read, which
looks like regen) - likely the packet carrying the hero's own HP/MP/CP,
useful for the next item on the roadmap below.

## Hero's own current HP/MP (session 9): `0x18`'s third shape

The loose end from session 8 turned out to be exactly what it looked
like. `0x18` fires a third way, tied to the hero's own id, always 28
bytes long, with a flag byte of `0x03` at offset 12 (vs. `0x01`/`0x02` for
target HP): `targetId=heroId, 00000000, 01, 03, <A: constant>, 09,
<current HP>, 0b, <current MP>`. Confirmed with a deliberate two-phase
capture:

1. **Idle, no combat**: both values climbed together by a fixed small
   amount per ~3s tick - consistent with passive HP and MP regen.
2. **Toggling a continuous-drain skill** ("Arcane Power", whose tooltip
   says "HP will be continuously consumed while in effect" and lists an
   MP cost of 36): the HP-shaped field showed a clean sawtooth - drop by
   the skill's per-tick drain, climb back by the regen amount, repeat -
   and the MP-shaped field dropped by **exactly 36** in the single packet
   right after the skill was cast, then regenerated straight back to its
   resting value.

`PacketHandler::GetHeroCurrentHp()`/`GetHeroCurrentMp()` expose this;
`HeroRepository` feeds it into `VitalStats` (with `maxHp`/`maxMp` left at
0 - unknown).

**Max HP/MP/CP remain unsolved.** Two different memory scans (bounded to
`GetMyPawn()`'s first 0x600 bytes, then widened to 0x4000) for the exact
values read off the live UI (5767/3585/1516) found neither - unlike the
network id at `+0x58`, these aren't sitting in that struct as plain
dwords, at least not within the scanned range. The likely real source is
`0x0b`, the packet session 5 found carrying our own name at login (see
the hero-self-identification section above) - its other fields were
never mapped past the name, and that's almost certainly where max stats
live, sent once at EnterWorld rather than repeated like current HP/MP.
Needs a fresh login capture and the same kind of manual decode this
document has done for every other packet, not started this session.

**Resolved, same session**: that "unexplained" dword wasn't one field, it
was two. Reading it as one byte (`0x21`, still unexplained) followed by a
3-byte little-endian value gives **1516** - current CP, constant because
the hero's CP stayed full (1516/1516) throughout every capture so far,
never confirmed decreasing. `GetHeroCurrentCp()` exposes this the same
way as HP/MP.

## Max HP/MP/CP: a real dead end, not just "not found yet"

Three independent searches for the exact values shown on screen
(5767/3585/1516), none finding anything:

1. `GetMyPawn()`'s struct, scanned as 32-bit values, bounded to 0x600
   bytes then widened to 0x4000 (session 9).
2. A fresh EnterWorld capture of `0x0b` (the packet confirmed to carry our
   own name, 193-205 bytes) - searched as 32-bit, 16-bit, and
   IEEE-754-float, every byte alignment, little- and big-endian. Nothing.
3. The one remaining unidentified packet from that same login burst
   (`0x0e5`, 63 bytes) - its few nonzero fields are small paired values
   (180, 120, 175) that look like resistances, not stat caps.

The likely explanation: max HP/MP/CP for a *player* character aren't
transmitted as literal numbers at all in this architecture - standard
classic-L2 client design derives them client-side from base stats
(STR/CON/MEN/INT/DEX/WIT), class and level via a formula, the same way
the Shift+2-click monster-info window reads a monster's stats from local
`.dat` tables instead of the network (session 7). Getting a real max
value would mean reverse-engineering that formula (and the per-level/
per-class tables it reads), which is a different kind of task from
everything else in this document - not attempted.

## Roadmap — ordered so nothing already built has to be reworked

Goal, stated plainly: feature parity with `ciws/L2Bot`'s full Interlude
support (HP/CP/MP/level/name/XP%/Adena, inventory, skills, items, other
players, NPC name+level+HP) on *this* client, then the same for every
other client family the server runs (Classic/H5/Essence/...), with a
detection layer that picks the right `VersionAbstractFactory`
automatically. This is a program, not a next step - sequenced below by
real dependency, not just importance, specifically so a later item never
forces reopening an earlier one.

### Rules this order is built from (every one cost a real detour this session)

- **Correlate against an exact on-screen number whenever one exists.**
  Pattern-matching ("looks about right") produced two false leads this
  session (the frequency-based hero-id guess, and misreading `0x18`'s
  current-value pair as current/max); an exact match (the Buffalo's 8170,
  the skill's 36 MP cost) settled things in one capture.
- **Isolate the capture.** Any session with other players or ambient
  mobs nearby pollutes frequency-based and even some value-based reads
  (the mob-attacking-many-players case). A single hero vs. a single,
  otherwise-idle target is worth the extra setup time every time.
- **A login/EnterWorld-only packet needs a login/EnterWorld-only
  capture.** Normal-gameplay captures will never contain it - start
  `Client.exe`'s hook *before* clicking into the character, every time
  one of these is the target.
- **Memory reads only work through a *named, exported* accessor
  (`GetMyPawn`, `GEngine`).** There is no generic "read any object by its
  network id" - that id is not a dereferenceable pointer for anything
  except what `GetMyPawn()` itself returns (tested and falsified this
  session). Don't reach for a memory scan without one of these first.
- **One opcode can have several shapes.** Always check a header flag
  byte before trusting a fixed offset layout - `0x18` alone has three.
- **A value that never moves across a whole session is probably "the
  character didn't use that resource," not "nothing interesting here."**
  CP looked like a decoration for two sessions before a byte-split read
  gave it meaning.

### Ordered plan

**0. Already done, do not redo:** hero self-ID (memory), hero+NPC
position (`0x33`), NPC current/max HP (`0x18` target shape, `0x0c`
name/alive), hero current HP/MP/CP (`0x18` hero shape). All wired through
`Moonland::PacketHandler` into `NPCRepository`/`HeroRepository`.

**1. Finish decoding `0x0b` (own CharInfo) for level, class, race, base
stats.** Already captured and partially mapped (name, a small object-like
id at +19 that is *not* the network id, several small int fields) - this
is refining an existing capture, not a new hunt. Confirmed **not** to
carry max HP/MP/CP (exhaustively searched, see above), but level/class/
race are small, plausible-looking fields already sitting in that same
dump; a fresh EnterWorld capture plus checking the in-game character
sheet for exact level/class/race values would nail them down the same
way CP was found. Do this first: the hero's own name and level are
needed to label *anything* sensibly, including every later item below.

**2. NPC level and hostility flag.** Both likely already visible, unused,
in `0x0c`'s existing dump (a small int near the name, and the `01`/`02`
flag byte at offset 7 that was ignored when writing the NpcInfo parser).
Check the existing capture data before opening the client again.

**3. Skill/magic attack opcode.** Confirmed gap: `0x33` is physical-attack
-only; a caster's skill hit produced real damage and zero `0x33` packets
in session 6. Needed before combat tracking can be called complete, and
before loot/XP (item 5) can be attributed correctly to skill kills, not
just melee ones. Isolated single-target capture, skill-only hits, same
method as `0x33` was found.

**4. Hero death/respawn handling.** `PacketHandler` has no "hero died"
signal yet - HP hitting 0 via the shape found in item 0 is the
likely trigger, or a dedicated opcode. Do this before anything that reads
hero state continuously (farming loops, future), so death doesn't show
up as a silent stuck state.

**5. Own XP/SP and level-up.** Needs either a dedicated
"experience gained" opcode (not found - would show up right after a
kill, isolated single-kill capture) or confirming it's part of the item-1
CharInfo refresh. Depends on item 1's decode being done first, so XP's
byte-shape can be told apart from the fields already identified there.

**6. ItemList (`0x11`) full decode - inventory and Adena.** The opcode is
already confirmed structurally (repeating fixed-size records, session 5)
but the per-record fields (itemId, count, enchant, equipped flag) were
never mapped to exact offsets. Adena is just itemId `57` in the same
list - no separate hunt needed once this is decoded.

**7. SkillList (`0x5f`) full decode.** Same situation as item 6: opcode
and repeating `(level, skillId)` shape already confirmed (session 5),
exact field boundaries not yet nailed down.

**8. Other players (`0x31` CharInfo, for ids other than the hero).**
Already decoded structurally (name + position + a large stat block,
session 2/5 samples), never wired into a repository. Lower priority than
the above because the bot's own combat/inventory loop doesn't depend on
seeing other players, but reuses the exact same `NPCRepository` pattern,
so cheap once items 1-7 establish the field-decoding conventions this
client uses.

**9. Multi-client generalization - only after 1-8 are solid for this one
client.** Repeat this entire methodology against each other client family
Moonland (or whichever target server) actually runs, producing a second
`Versions/<ClientFamily>/` tree the same shape as `Versions/Moonland/`.
Only then build the detection layer (by client file hash, a handshake
packet's version field, or probing which `Engine.dll` exports survive -
the same technique that found `GetMyPawn`) to pick between them
automatically in `VersionAbstractFactory::GetFactory`, replacing
`dllmain.cpp`'s current hardcoded `Version::classic`. Doing this before
even one client is feature-complete would mean redesigning the detection
layer every time a new per-client quirk shows up - this order avoids
that entirely.

Each numbered item is its own session (or several); none should start
until the ones before it that it depends on are done.

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
