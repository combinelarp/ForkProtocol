# ForkProtocol

*Made by reloaded labs*

Lets the stock **10068** gameserver talk to the **10096 (2023 fork) client**.
The fork renumbered and expanded a bunch of packets, so a vanilla 10068 server
cannot read what the fork sends, and the fork cannot read what the stock server
answers. This shim sits between the two and translates.

The 10096 client is **prebuilt and cannot be patched** — every workaround here is
server-side.

---

## Status at a glance

| # | Issue | State |
|---|-------|-------|
| 1 | Game-ending **score / settlement UI pops up when a *different* player joins** | **OPEN — still reproduces** (fix attempt did not work) |
| 2 | **Leaderboard** does not work | OPEN — reported, details TBD |
| 3 | `game.exe` stuck on *"Loading Player Data..."* | Patched, not verified in-game |
| 4 | Movement bugs | Wired, not verified |
| 5 | Other 10096 players not replicating | Wired, unverified |
| 6 | Respawn does not use the saved position | OPEN |
| 7 | S2C packet ids the 10096 client does not know (e.g. 111) | Handled per-id, watch for more |

---

## Main pieces

```
ForkProtocol.h                     C2S gateway + the expanded login handshake
ClientPeer.cpp / .h                login, DBDataReady, respawn position
ServerNetwork*.cpp                 packet routing / session tracking
ServerPacketSender1..5.cpp         S2C packet builders
C2SPacketHandles/                  per-packet C2S handlers (ClientReady etc.)
ServerCommonDataSender.cpp         generic key/value (S2CPacketServerCommonData) channel
```

Server-side game rules live in Lua (`res/client/Media/Scripts/ServerGame/**`) and
push their own UI through `HostApi.*` / `PacketSender:*`.

---

## OPEN ISSUES

### 1. Game-ending score / settlement UI appears when *another* player joins  ⚠️

**Symptom**
- A player who is already in the room suddenly gets the post-game "results /
  score" screen (`gui_deadSummary` / `gui_finalSummary` on the client).
- It fires when **a different player joins** the room.
- It also re-appears after clicking **"Next game"** on that screen.
- Closing / returning from that screen **crashes the prebuilt 10096 client**
  (the only defence is to never deliver it spuriously).

**What actually triggers it**
- It is **not** only the `PlayerSettlement (66)` / `GameSettlement (67)` path.
  Gating pids 54 / 66 / 67 / 164 did **not** stop it.
- The same screen also arrives over the generic key/value channel
  `S2CPacketServerCommonData` (**pid 1**). The client hands it to
  `CommonDataDispatch -> "Lua" -> ServerCommonDataEvent`
  (`S2CCommonDataPacketHandles.cpp:134`) and that opens the same GUIs.
- The server-side game Lua reaches that channel with
  `PacketSender:sendLuaCommonData(rakssid, "<settlement key>", data)`.
  Known settlement keys seen in the mode scripts:
  - `"DeadSummaryData"`   — SkyWarLuckyBlock
  - `"SingleSettlement"` / `"KeyAdSettlementSingle"` — BedWar
  - `"DeathSettlement"`   — WalkingDead
- The pid-based settlement senders (`ServerPacketSender5.cpp` 54/66/67,
  `ServerPacketSender2.cpp` 164) and these common-data keys all funnel through
  `ServerNetwork::sendPacket`.

**Where the "match over" comes from**
The mode scripts end a match through `GameMatch:doReward()` ->
`sendGameSettlement()` (all players) and `GamePlayer:reward()` ->
`sendPlayerSettlement()` (per player, when the match is *not* over). A join is
landing in a code path that decides the match ended and pushes the score screen
to the already-present player.

**Fix attempted (did NOT resolve it)**
A "join-window" choke was added so that only the spurious join-triggered
settlement is dropped and the genuine end screen still gets through:

- `ForkProtocol.h`: `RecentJoins` singleton + `markPlayerJoined(nowMs)` +
  `settlementSuppressedByJoinWindow(nowMs)` with `kJoinSettlementGraceMs = 20000`.
- `ClientPeer::login()` calls `ForkProtocol::markPlayerJoined(...)`.
- `ServerNetwork.cpp` `isForkBlockedResultPacket()`: for fork sessions, pids
  54/66/67/164 **and** the settlement `ServerCommonData` keys are dropped **only
  while a join happened within the last 20 s**, otherwise forwarded.
- The unconditional "never send settlement to a fork" early-returns in
  `ServerPacketSender5.cpp` / `ServerPacketSender2.cpp` were removed (they were
  bypassing the choke and would also have killed the genuine end screen).

**Result: the screen still appears.** So either
(a) the spurious packet arrives **later than 20 s after the join**,
(b) the triggering packet is **not** one of the ones currently classified
    (unknown pid or unknown common-data key), or
(c) it is delivered by a path that **bypasses** `ServerNetwork::sendPacket`.

**Next steps / how to narrow it**
- The choke logs `[FORK] join-triggered settlement ... suppressed for fork
  session <id>` whenever it drops something. Reproduce and check the server log:
  - Log **present** but screen still shows -> the *window* is too short, or the
    same UI is opened by a second, unclassified packet.
  - Log **absent** -> the packet is not being classified. Capture which
    pid/key is actually delivered at that moment and add it.
- If a fresh capture is allowed: grab the datagram the already-present client
  receives at the moment the other player joins and read the leading varint pid.
- Consider gating by *room state* (match not yet started / just restarted)
  instead of by time, so a genuine end-of-match is never affected.

**Do not** simply blanket-block every settlement forever — the requirement is
that the **real** end screen *must* still fire. That is why the choke is
conditional.

---

### 2. Leaderboard

Reported: the leaderboard / rank board does not work on the 10096 client.
No leaderboard-specific code exists in `server/src` or the ServerGame scripts,
so this is fed entirely by packets the core sends (rank / score / settlement
data). It very likely shares the settlement path above — while settlements are
suppressed / mis-timed, the rank data that fills the board is missing too.

> TODO: confirm the exact symptom (empty board / wrong entries / crash) and
> whether it is the in-game rank list or the end-screen rank list, then find the
> packet that carries it and check it is not being dropped for fork sessions.

---

### 3. `game.exe` stuck on "Loading Player Data..."

Root cause found and patched, **NOT VERIFIED IN-GAME**. See the section below.

---

### 4. Movement bugs

Not finished. Fork id **8 = PlayerMovement** (stock id 8 = VehicleMovement) is
decoded by `decodeForkMovement()` and injected as a stock
`C2SPacketPlayerMovement`. Anti-cheat is bypassed **only** for sessions already
classified as fork.

---

### 5. Other 10096 players not replicating

Wired but unverified. The ClientReady handler sends a directed `UserIn` for
every other logged-in peer, then `EntityTracker::forceTeleport()` re-spawns
everything around the newly-ready client. Needs a two-client test.

---

### 6. Respawn does not use the saved position

`setRespawnPos()` is never called. `ClientPeer` has the plumbing but the respawn
still drops the player at the default position.

---

### 7. Unknown S2C packet ids

Any S2C id the 10096 client does not know makes it log
`unknown packet id: <id>` and then **misparse the rest of the stream**
(garbage ids, decode errors, crash). Example already handled:
- **111 `SyncBlockDestroyeFailure`** — does not exist in 10096; never send it to
  a fork session (`ServerPacketSender5.cpp::sendBlockDestroyeFailure`).

Watch for more of these; each must be either remapped or suppressed per session.

---

## FIXED BUGS

- Login blob was **1 byte too long** and mis-aligned the whole fork profile.
- Login blob was sent **twice** per session.
- `DBDataReady (id 348)` delivery.
- Fork movement (id 8) was dropped on the floor.
- Reconnect with a reused RakNet id kept stale fork session state.
- `UserIn` of already-logged-in peers was never sent to the new client.

---

## Loading Player Data — root cause

The fork client only fires `C2SPacketClientReady` once its expanded
`LoginResult (id 50)` parses cleanly. Ours did not.

Byte-diffed against the working capture (`newdump.pcapng`, frame 208, a single
165-byte login blob). Three mismatches, all in the id-50 body:

1. **`kMidSig` hardcoded to `{00 00 3d c0 48}`.**
   The trailing `3d c0 48` is a real embedded `S2CPacketChangePlayerInfo` (pid
   61): `3d` = pid, `c0 48` = zigzag varint playerId. `c0 48` decodes to a
   garbage playerId. The reference has `3d 1c` (playerId 14 = the player's real
   entityId). Ours was therefore one byte longer than the reference and the
   fork's expanded-LoginResult parser lost alignment from there on.

2. **The blob was emitted twice per session** — once as the answer to the
   client's id 179 handshake, once from `ClientPeer::sendLoginResult`.
   `huysa.pcapng` shows 386 bytes / 2 blobs / 3x DBDataReady; the working
   reference shows one clean 165-byte blob.

3. **A trailing `dc 02` (DBDataReady) was appended inside the id-50 body.** The
   reference does not have it, and DBDataReady already reaches the client at
   offset 317 of the GameInfo batch plus as standalone messages.

Fixes:
- `buildLoginResultBody()` now emits `3d` + zigzagVarint(entityId) so the
  embedded ChangePlayerInfo carries the correct playerId.
- new `appendZigzagVarint()` helper (the client reads i32 as zigzag + varint).
- trailing inline `dc 02` removed; blob now ends on `kTail` like the reference.
- `sendLoginPackets()` restored its once-per-session `markSent()` guard.
  `clearSession()` drops the mark on disconnect, so reconnects still work.
- `sendLoginPacketsForRakid()` resolves the peer's entityId and refuses to send
  (without consuming the mark) if the player has not spawned yet.

Verified: the new encoder was re-implemented independently and reproduces the
reference login body byte-for-byte (164/164 bytes, IDENTICAL).

---

## DBDataReady delivery (id 348, empty body)

Sent 3 ways, all idempotent on the client:
- inline at offset 317 of the GameInfo batch (`sendWorldInfo`)
- `ClientPeer::login()`, the moment the Lua `PlayerLoginEvent` succeeds
- `ClientPeer::sendLoginResult()`, AFTER the player profile

Must stay last: it flips `GameClient::isDbDataReady()`, which gates
`Blockman::onGameReady() -> C2SPacketClientReady`.

---

## Gotchas

- Do **NOT** send a standalone `S2CPacketUserIn` to the connecting player during
  login. The fork's UserIn handler dereferences `m_pWorld` unconditionally
  (`S2CPlayerActionPacketHandles.cpp:491-492`) and `m_pWorld` is still null then,
  which crashes WinShell. The player's own profile rides **inline** inside the
  expanded id-50 body instead.
- The client's decode loop **ABORTS the rest of a RakNet message** on the first
  packet it cannot decode (`RaknetInstance.cpp:496`). Anything after a bad packet
  in the same message is silently dropped. For fork sessions we send one app
  packet per RakNet message (`isolatePackets`) so a single bad packet cannot
  desync a whole batch (leftover bytes used to be re-read as fake ids 66/67 —
  the score screen).
- `"UseForkProtocol"` pref is dead — `isEnabled()` is always false. The shim is
  keyed off an id **179** on the wire instead.
- Stock anti-cheat rubber-bands / kicks fork movement. It stays **ENABLED** for
  everyone except sessions already classified as fork.
- Fork classification is cleared on disconnect (`ForkProtocol::clearSession`) so
  a reused RakNet id is not stranded on "Loading player data...".

---

## Build

```
MSBuild.exe ".\dev\server\Build\src\GameServer.vcxproj" ^
    /p:Configuration=Debug /p:Platform=Win32 /m /nologo
```

The build script also builds `logic\Build\Src\Logic.vcxproj` **first** —
`GameServer.vcxproj` has no project reference to Logic, so Logic must be rebuilt
explicitly before GameServer (otherwise you get LNK2019 for Logic symbols such as
`RaknetInstance::isolatePackets`).

Look for this in the server log after connecting:

```
C2SPacketClientReady---->UserId: <uid> Ready
```
