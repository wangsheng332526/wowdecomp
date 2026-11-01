# Character Hot-Swap ("GTA-style") — Implementation Plan

Status: **Phase 1 and the swap transition (§13, §14.1) are built and in use on the dev realm.** Shadows (phase 2) and
the in-place swap (§14.2) are not started.
Server core: `R:\Users\Zach\Documents\GitHub\EsteriaWoW` (AzerothCore-derived). Client: this repo.

## 1. Goal

While in the world, pick another character on your account from a wheel and become them within seconds, without visiting the character-select screen. The character you leave stays in the world as a **shadow** that everyone can see.

Decisions already made:

| Topic | Decision |
|---|---|
| Scope | Swap anywhere, but only at "safe moments" (§5) |
| Shadows | Persist and are visible to everyone |
| Parked behaviour | Walk to the nearest inn/capital if close enough, otherwise stay in the tight vicinity |
| Shadow cap | 4 per account |
| Swap cooldown | 30 s |
| Party-style simultaneous play | **Out of scope** (not doing the Genshin-style mode) |
| Cluster / ToCloud9 mode | Not supported. Swap refuses if `ClusterModeEnabled()` is true. No cluster settings were found in `conf/`. |

## 2. What the code already gives us

Verified by reading the server source.

| Fact | Where | Why it matters |
|---|---|---|
| `LogoutPlayer(bool save, bool redirecting)` does the full teardown: loot release, BG/queue leave, guild notice, pet removal, group handling, `SaveToDB`, channel cleanup, friend-offline broadcast, `RemovePlayerFromMap`, then sends `SMSG_LOGOUT_COMPLETE`. `redirecting=true` skips group removal, the friend-offline notice, `OnPlayerLogout`, and the "set all account characters offline" SQL. | `src/server/game/Server/WorldSession.cpp:704` | This is the teardown for the old character. `redirecting` already models "log out here, log in elsewhere on the same account". |
| `HandlePlayerLoginOpcode` kicks if the session already has a `Player`. Otherwise it builds a `LoginQueryHolder` and calls `HandlePlayerLoginFromDB`. | `Handlers/CharacterHandler.cpp:970` | The swap must finish logging the old character out before the login. |
| **Takeover of an in-world character already exists.** If the account has an offline session holding that exact character, login does `sess->SetPlayer(nullptr); SetPlayer(p); p->SetSession(this); HandlePlayerLoginToCharInWorld(p)`. | `CharacterHandler.cpp:~1010–1075` | This is the shadow-takeover path. We reuse it instead of loading a shadow from the DB. |
| Offline sessions are keyed by **account id**, one per account. Logging in erases the account's existing offline session. They are removed after 60 s. | `WorldSessionMgr.cpp:73, 152–161, 187–197, 347–352` | The existing map cannot hold 4 shadows per account and expires them in 60 s. We need our own container. |
| Custom Esteria opcodes already exist (`0x522–0x526`), with handlers in `CharacterHandler.cpp` and `Opcodes.cpp`. | `Opcodes.h:1344–1348` | Pattern to copy for the new opcodes. |
| Client extension framework: `RegisterMessageHandler`, `RegisterLuaFunction`, `RegisterFrameScriptEvent`. | `Extensions/DigitalInventory/src/DigitalInventory.cpp:225–228` | Pattern to copy for the client side. |
| Client handles `SMSG_LOGOUT_COMPLETE` in `RealmConnection` (returns to glue). | `WoW/Source/WowServices/RealmConnection.cpp:163, 214` | Hook point for the "auto re-enter" logic. |
| `mod-playerbots` is present (socketless sessions, `OnPlayerbotLogout` hook called from `LogoutPlayer`). | `modules/mod-playerbots`, `WorldSession.cpp` | Prior art for socketless `Player`s. We will **not** depend on its AI; see §6. |

## 3. Architecture overview

```
Client                                   Server
------                                   ------
Wheel UI (Lua)  --CMSG_ESTERIA_SWAP_REQUEST(guid)-->  SwapGuard: validate
                                                        |  ok
                <--SMSG_ESTERIA_SWAP_RESULT(ok)---------+
cinematic starts                                        |
                                           Phase 1: LogoutPlayer(save=true)
                <--SMSG_LOGOUT_COMPLETE-----------------+
client auto-sends CMSG_CHAR_ENUM, then CMSG_PLAYER_LOGIN(guid) (no glue UI)
                                           login (marked as a swap login)
                <--SMSG_LOGIN_VERIFY_WORLD + normal login burst
cinematic ends                                          

Phase 2 replaces "LogoutPlayer" with "Park as shadow" and adds
"take over a shadow" in the login path.
```

Key idea for phase 1: the server only adds a **validated, cooldown-stamped logout** and a **swap-login flag**. The existing login protocol is untouched, so the client keeps working with the normal login burst.

## 4. Protocol

New opcodes (next free numbers after `0x526`; confirm they are unused before assigning).

| Opcode | Dir | Payload | Purpose |
|---|---|---|---|
| `CMSG_ESTERIA_SWAP_REQUEST` | C→S | `guid(8)` | Ask to become this character |
| `SMSG_ESTERIA_SWAP_RESULT` | S→C | `guid(8) result(u8) cooldownMs(u32)` | `0` ok, `1` not your character, `2` cooldown, `3` unsafe (reason in `detail`), `4` target is online elsewhere, `5` disabled/cluster mode, `6` internal error |
| `CMSG_ESTERIA_SWAP_ROSTER_REQUEST` | C→S | none | Wheel opened |
| `SMSG_ESTERIA_SWAP_ROSTER` | S→C | `count(u8)` then per character: `guid, name, race, class, gender, level, mapId, zoneId, hpPct(u8), flags(u8: isShadow, isActive, isDead), cooldownMs` | Data for the wheel |
| `SMSG_ESTERIA_SWAP_UNSAFE` (optional) | S→C | `reasonCode(u8)` | Lets the wheel grey out the swap before the player clicks |

Roster stays small so it can be re-requested each time the wheel opens. No periodic pushes.

## 5. Safe-moment rules

Evaluated server-side, in the swap handler, on the current `Player`. The client may pre-check for UX, but the server is authoritative. GMs bypass.

Refuse when the current character is:

- in combat, or recently damaged by a player (PvP timer)
- casting or channeling
- dead, a ghost, or in Spirit of Redemption
- in a battleground, arena or any instance, or queued for one (incl. LFG queue)
- mid-teleport, in a taxi flight, or on a vehicle
- in a duel, trading, or with a loot window open
- holding a pending bind
- in a **raid** group (see Decisions §11)

Also refuse when: the account swapped within the last 30 s; the target is currently active, or is another account's character; the target character's save is pending; swap is disabled in config; cluster mode is on.

Each rule maps to an existing check that `LogoutPlayer` already makes (BG leave, queue removal, `IsBeingTeleportedFar`, etc.), so those cases are refused instead of being quietly "fixed up".

## 6. Server design

### 6.1 New files (core, since it touches session internals)

- `src/server/game/Server/CharacterSwap.h/.cpp` — `SwapGuard` (validation, cooldowns, save-pending set) and `ShadowMgr` (phase 2).
- Opcode handlers next to `CMSG_ESTERIA_FORM_LOOK` in `CharacterHandler.cpp`; entries in `Opcodes.h`/`Opcodes.cpp`.
- Config in `worldserver.conf.dist`:

```
Esteria.Swap.Enable            = 0        # kill switch, default off
Esteria.Swap.CooldownSeconds   = 30
Esteria.Swap.MaxShadows        = 4
Esteria.Swap.ShadowTimeoutMin  = 30
Esteria.Swap.OwnerLogoutGraceSec = 60     # shadows leave this long after the owner's session ends
Esteria.Swap.ShadowWanderMaxYd = 400      # path length to an inn/capital
Esteria.Swap.AllowRaidGroup    = 0
```

### 6.2 Phase 1 flow

1. `CMSG_ESTERIA_SWAP_REQUEST(target)`:
   - Run all §5 checks. On failure send `SWAP_RESULT` with the reason.
2. On pass:
   - Stamp the cooldown on the account, set `session->_swapTarget = target`.
   - Call `LogoutPlayer(true)` (with `redirecting` left false in phase 1, so group leave and friend-offline behave as a normal logout; there are no shadows yet, so the party must not keep a logged-out member).
   - Reply `SWAP_RESULT(ok)`. The standard `SMSG_LOGOUT_COMPLETE` follows from `LogoutPlayer`.
3. Client re-enters the world (§7).
4. `HandlePlayerLoginOpcode`: if `guid == session->_swapTarget` and within ~30 s, mark the new `Player` as a swap login (`Player::SetSwapLogin(true)`) and clear `_swapTarget`. A login for any other guid while `_swapTarget` is set simply clears it.
5. Swap-login effects: skip MOTD and server-info lines, skip friend/guild "online" announcements, set a flag that `OnPlayerLogin` module hooks can read (see §6.5).

### 6.3 Save-race lock (the dupe guard)

`SaveToDB` on logout is asynchronous, while `LoginQueryHolder` reads on a separate DB path. A→B→A quickly could load A before A's save commits.

- Keep `std::unordered_set<ObjectGuid::LowType> _savePending`.
- Save with an explicit transaction and an async-commit callback that removes the guid from the set. Chain the login holder after that callback when the target is in the set. **To verify:** which `SaveToDB` overload exposes the transaction, and how `AsyncCommitTransaction` callbacks are processed.
- The 30 s cooldown already makes the race very unlikely, but the lock must exist anyway. Crashes, lag and GM commands bypass the cooldown.

### 6.4 Phase 2: shadows

**Parking** (replaces `LogoutPlayer` in the swap handler):

1. Run the same pre-park work the logout does for a character that stays: leave channels that must not persist, dismiss pet, remove map-change auras, save to DB. The character **stays in its party** (§11); do not call `RemoveFromGroup()`.
2. Create a socketless `WorldSession` for the account (modelled on how `mod-playerbots` builds its sessions; **to verify** the constructor signature).
3. Move the player: `session->SetPlayer(nullptr); shadow->SetPlayer(P); P->SetSession(shadow);` (same pattern as the existing takeover code, reversed).
4. Register it in `ShadowMgr`: `map<accountId, vector<Shadow>>`, where `Shadow` holds the session, park time, and wander state.
5. If the account already has 4 shadows, fully log out the **oldest** one first (`LogoutPlayer(true)` on its session). Save before dropping.

**Shadow state:**

- AFK flag with a generic message such as "Resting." It must not mention the owner. Whispers get the standard AFK auto-reply.
- Non-attackable and immune to hostile actions while parked. No loot, no trade, no duel, no group invite.
- Position and `characters.online = 1` are kept so the who-list, friends and guild show them online.
- Do not accrue played time.

**Updating:** `ShadowMgr::Update(diff)` called from `World::Update` (or a `WorldScript::OnUpdate`). It drives shadow sessions, wander logic and the timeout. Map updates the `Player` itself.

**Taking over a shadow** (swap-back):

- In the swap handler, find the target in `ShadowMgr` first.
- Skip the cinematic round trip through glue where possible: stop movement, detach the shadow session, then run the existing takeover branch (`SetPlayer`, `SetSession`, `HandlePlayerLoginToCharInWorld`). No DB reload, so no stale-row race.
- Phase 1's client flow (logout → auto-login) still works for this case. The server just resolves the login from `ShadowMgr` instead of the DB.

**Lifetime:**

- Despawn after `ShadowTimeoutMin`, via the normal `LogoutPlayer(true)` on the shadow session.
- Default: shadows are also logged out when the owner's real session ends (plus a short grace). This avoids a world full of idle characters belonging to offline players. See §11.
- Account banned or kicked → log out all its shadows.
- Server shutdown → `ShadowMgr::LogoutAll()` before `KickAll()`.

**Wandering:**

- On park, pick the nearest destination on the same map: tavern trigger (the `areatrigger_tavern` table plus the areatrigger DBC for coordinates) or capital area.
- Build a path with the existing mmaps `PathGenerator`. If the path is longer than `ShadowWanderMaxYd` or no path exists, stay within ~10 yd of the park point.
- Move with `MotionMaster::MovePoint`; on arrival run a small idle loop (sit, sleep in an inn, occasional emote).
- Keep it ~150 lines. Deliberately **not** using the playerbots AI, to avoid coupling to a 200 KB module. Self-defence can come later.

### 6.5 Audit list (things that assume one online character per account)

- Modules keyed by account id, or that call `FindPlayerByAccountId`-style lookups. Candidates by name: `mod-account-mounts`, `mod-account-achievements`, `mod-individual-progression`, `mod-individual-xp`, `mod-premium`, `mod-challenge-modes`, `mod-mythic-plus`.
- Any `OnPlayerLogin` / `OnPlayerLogout` hook that sends a greeting, grants a reward or resets state. Gate on the swap-login flag.
- `mod-playerbots`: confirm it treats a socketless non-bot `Player` correctly (it should find no bot AI).
- Realm population / session counters, if used for queues.
- The "set all characters of the account offline" SQL that runs at non-redirecting logout. Make sure it doesn't mark shadows offline.
- Anything counting `Player::GetSession()->GetSocket()`-less players as bots or disconnected.

## 7. Client design

### 7.1 Extension `CharacterSwap`

New directory `Extensions/CharacterSwap/` modelled on `Extensions/DigitalInventory/` (README, `src/`, `tests/`):

- `RegisterMessageHandler` for `SMSG_ESTERIA_SWAP_RESULT`, `SMSG_ESTERIA_SWAP_ROSTER`.
- Lua functions: `EsteriaSwap_RequestRoster()`, `EsteriaSwap_GetRoster()`, `EsteriaSwap_Request(guid)`, `EsteriaSwap_GetCooldown()`.
- Frame-script events: `ESTERIA_SWAP_ROSTER`, `ESTERIA_SWAP_RESULT`, `ESTERIA_SWAP_COOLDOWN`.

### 7.2 Auto re-entry (phase 1)

- When `SWAP_RESULT` is ok, store `g_pendingSwapGuid`.
- When glue reaches character select with a pending guid: do not show the UI, call the existing "enter world" path with the character whose guid matches, and show a loading mask (the transition covers it).
- If the guid is not in the enum, or login fails, clear the pending state and show the normal screen.
- **To locate:** the exact enter-world function in `WoW/Source/Glue/CGlueMgr.*` / `CharacterSelection.cpp` (we recently modified the latter for zoom/pan), and the right place to hook `RealmConnection` after `SMSG_LOGOUT_COMPLETE`. Use the `wow_x86` / `wow_x64` MCP tools to confirm.

### 7.3 Wheel UI

- A Lua/XML frame shipped in the patch MPQ via the existing EsteriaShell tooling (`Tools/EsteriaShell`), and a keybinding entry.
- Hold-to-open radial, one slot per character, with class icon, name, level, zone, health, and badges for "shadow", "dead", "cooldown".
- Phase 1 uses class/race icons. 3D model portraits can come later, from roster appearance data.
- Greys out unsafe states using `SMSG_ESTERIA_SWAP_UNSAFE` or the roster flags.

### 7.4 UI reload

A fresh login in the same process re-runs the UI as on any login, so addons and SavedVariables behave as they do after a normal relog. Phase 2/3 may skip the glue round trip and then needs an explicit "reload UI" step with SavedVariables flushed first.

### 7.5 Cinematic (phase 3)

- Same-map swap: pull the camera up and away, drift, descend onto the new character. No load required.
- Different-map swap: fade through clouds while the login completes.
- Camera code is reused from the select-screen zoom work; the in-world camera needs an override hook.

## 8. Phases and acceptance criteria

### Phase 0 — read-only spike (small)

- Confirm `SaveToDB` transaction access, `WorldSession` constructor, hooks to attach to `World::Update`, and the client enter-world function.
- Confirm no conflicting `CMSG`/`SMSG` numbers.
- Output: a short note appended to this document.

### Phase 1 — instant relog (medium)

Deliver: opcodes, `SwapGuard` with safe-moment checks and cooldown, validated logout, swap-login flag, client extension, auto re-entry, basic wheel.

Accept when:
- Swap A→B→A works with the 30 s cooldown and no item/gold/position loss.
- Every §5 refusal is hit by a test and refuses with the correct reason.
- No "online" spam to friends/guild, no MOTD, no module greeting on a swap.
- Disconnect during the swap leaves a consistent state (character saved, no stuck session).

### Phase 2 — shadows (large)

Deliver: `ShadowMgr`, parking, cap of 4 with oldest-out, takeover on swap-back, timeout, owner-logout cleanup, wandering, audit fixes.

Accept when:
- Another player sees the shadow, can whisper it (auto AFK reply), cannot attack, trade or invite it.
- Swap-back resumes the exact in-world state (auras, position, health) without a DB reload.
- The fifth park logs out the oldest and saves it.
- Server restart with shadows present saves and removes all of them cleanly.
- Owner logout removes shadows after the grace period.
- Shadows walk to a nearby inn when the path is short and stay put when it's long.

### Phase 3 — polish (medium)

In-place swap with no glue round trip, hover pre-fetch of the login holder, the camera cinematic, model portraits on the wheel.

## 9. Exploit and risk review

| Risk | Mitigation |
|---|---|
| Item/gold duplication via save race | `_savePending` lock chained to async commit; takeover from live shadow avoids reload |
| Escaping combat or death by swapping | Safe-moment rules; no swap when dead/ghost or in combat |
| Buff-relaying with a parked alt | Shadow is non-attackable; buffs from other players are normal gameplay. Revisit if abused. |
| Rested-XP farming by parking in inns | Equivalent to logging out in an inn. Accept, or suspend rest accrual for shadows. |
| Revealing alt ownership | Generic AFK text; no "owner" in any shadow-visible string; no announcements on swap |
| Idle characters of offline players filling the world | Owner-logout cleanup + 30 min timeout |
| Crash with shadows in memory | Every shadow is saved at park time, so a crash loses at most what a normal crash would |
| Module assumptions about one character per account | Audit list in §6.5 |
| Instance/BG state | Swap is refused there, so shadows only ever exist on open-world maps |
| Cluster mode | Feature refuses if `ClusterModeEnabled()`; revisit only if a gateway is ever used |

## 10. Testing

- Dev realm with one account holding 6 characters across 2 continents.
- Scripted scenarios: A→B, A→B→A, rapid repeat (cooldown), swap during every refused state, swap with dead target, 5th shadow overflow, disconnect mid-swap, server restart with shadows, ban with shadows, whisper a shadow, attack a shadow.
- GM commands for debugging: `.swap status`, `.swap shadows [account]`, `.swap release <account>`, `.swap cooldown reset`.
- Log every swap: account, from, to, outcome, elapsed ms.
- Rollout with `Esteria.Swap.Enable = 0` by default; enable on the dev realm first.

## 11. Decisions (resolved)

1. **Shadows when the owner logs out:** shadows leave 60 s after the owner's real session ends.
2. **Group handling on park:** the parked character **stays in the party** as a shadow; swapping is **refused while in a raid group** (`AllowRaidGroup = 0`). Interpretation to confirm: the character you swap *to* is not placed in that party. Consequences to handle in phase 2:
   - A shadow in a party auto-passes loot rolls and never blocks a roll.
   - Shadows must not count for party XP/loot sharing bonuses, or the party gets free split bonuses from an AFK member. Treat them as out of range.
   - Group-leader handling when the leader swaps away: transfer leadership to a connected member, or the shadow stays leader and is stuck. Transfer.
   - The shadow leaves the group when it is logged out (timeout, cap overflow, owner logout, shutdown), via the normal `LogoutPlayer(true)`.
   - **Phase 1 has no shadows**, so a phase-1 swap logs the old character out, and the existing `LeaveGroupOnLogout` behaviour applies. The party-keeping behaviour arrives with phase 2.
   - **Resolved:** swapping never places the new character in the shadow's party. If the two alts were already in the same party, they stay that way (same as a normal logout).
   - **Resolved:** a shadow behaves like an offline player: excluded from ready checks, summons, and similar group actions.
   - Shadow sessions are never registered in the main session map. If a login targets a character that is currently a shadow, the shadow is forced offline and the player replaces it (the §6.4 takeover path).
3. **Dead targets:** swapping *to* a dead or ghost character is allowed (only the *current* character being dead blocks a swap).
4. **Unsolicited logout:** the swap reply sends the same pair of packets as a normal instant logout (`SMSG_LOGOUT_RESPONSE` instant, then `SMSG_LOGOUT_COMPLETE`) so the stock client state machine is satisfied. Add a client shim only if Phase 0 shows that isn't enough.
5. **Wander threshold:** 400 yd of path length (`Esteria.Swap.ShadowWanderMaxYd = 400`). Tune in game.
6. **Rested XP for shadows:** allowed, same as logging out in an inn.

## 12. Not verified yet

- Behaviour of `SendPacket` and `LogoutPlayer` on a socketless session, other than that offline sessions already rely on it.
- The `SaveToDB` transaction overload and async-commit mechanics.
- The `WorldSession` constructor signature for a shadow session.
- The client's enter-world function and the glue hook point.
- Whether any listed module breaks with two online characters on one account.
- That the chosen opcode numbers are free.
- Nothing here has been compiled or run.

## 13. Phase 0 notes and Phase 1 build

### Phase 0 findings

- **Opcodes.** `0x527`–`0x52A` were free. `NUM_MSG_TYPES` is now `0x52B`.
- **Save race.** `SaveToDB(false, true)` builds and commits its own transaction, so no overload hands it out. The
  character DB has a single async queue (`DatabaseWorkerPool::_queue`). With `CharacterDatabase.WorkerThreads = 1`,
  which the Docker realm uses, the logout save is committed before the target's `LoginQueryHolder` runs. A normal
  logout and relog relies on the same ordering. Phase 1 refuses swaps (result 5) when `WorkerThreads > 1` instead of
  adding the `_savePending` lock. The lock is still needed for phase 2, or if the worker count is raised.
- **Client re-entry.** `HandleLogoutComplete` accepts an unsolicited `SMSG_LOGOUT_COMPLETE` and returns to character
  select. On show, the glue calls `GetCharacterListUpdate()`. That runs `CGlueMgr::GetCharacterList` and then
  `CCharacterSelection::UpdateCharacterList`. That function is the hook. `CGlueMgr::EnterWorld` is the enter-world path,
  and it is safe to call from there (`OnCharRenameResult` already calls it from a handler).
- **`SMSG_LOGOUT_RESPONSE`.** Optional. The handler sends it anyway, so the sequence is the same as the server's own
  instant logout.
- **Not checked yet (phase 2):** the `WorldSession` constructor for socketless shadow sessions, and the
  `World::Update` hook.

### What Phase 1 contains

Server (`EsteriaWoW`):
- `src/server/game/Handlers/CharacterSwapHandler.cpp`: request and roster handlers, the §5 checks and the cooldown.
- `Opcodes.h/.cpp`: four opcodes. `WorldSession.h`: `_swapTime`, `_swapTarget`, `_swapLogin`, `IsSwapLogin()`.
- `CharacterHandler.cpp`: `HandlePlayerLoginOpcode` marks a login as a swap login when the GUID matches within 30 s.
  Both login paths then skip the MOTD and the server-info line.
- `worldserver.conf.dist`: `Esteria.Swap.Enable` (default 0) and `Esteria.Swap.CooldownSeconds`.

Client (this repo):
- `Extensions/CharacterSwap/`: opcode handlers, Lua API and events. The README has the wire format.
- `WoW/Source/Glue/CharacterSelection.*`: `s_autoEnterGuid` and the auto-enter at the end of `UpdateCharacterList`.
- `Addons/EsteriaSwap/`: the wheel. Bind it under Key Bindings > Esteria, or use `/swap`. It is added to
  `Tools/EsteriaShell/build_shell_mpq.py`.

### Differences from the plan

- **`SWAP_RESULT`** carries a `detail(u8)` byte after `result`, so the unsafe reason can be shown.
- **`SMSG_ESTERIA_SWAP_UNSAFE` was not added.** The roster carries an `unsafe` byte instead.
- **Roster rows** have no `hpPct`. They add `zoneName`, because 3.3.5 Lua cannot turn a zone ID into a name.
  Roster flags are 1 active and 2 dead. There is no shadow flag until phase 2.
- **No `ESTERIA_SWAP_COOLDOWN` event.** The cooldown is in the result and roster events.
- **Friend and guild "online" notices are not suppressed.** A phase-1 swap is a normal logout, which already
  broadcasts "offline". Skipping "online" would leave friend lists and guild rosters wrong. Only the MOTD and server
  info are skipped. `IsSwapLogin()` is there for module hooks. None of the §6.5 modules are gated yet.
- **No PvP-timer check.** `IsInCombat()` covers PvP combat. Falling is refused, the same as for logout.
- **GMs** bypass the safe-moment checks only. The cooldown and ownership checks still apply to them.
- **The cooldown is per session.** A reconnect resets it. A reconnect also takes longer than the cooldown.
- **No loading mask over character select.** It shows for a moment before `EnterWorld` brings up the loading screen.
- **No `.swap` GM commands yet.** Each swap is logged under `entities.player` as `Swap: account … result …`.

### To test Phase 1

1. Rebuild the worldserver image and set `Esteria.Swap.Enable = 1` in `env/dist/etc/worldserver.conf`.
2. Rebuild the client, and rebuild the shell MPQ so it includes `Addons/EsteriaSwap`.
3. Run through the §8 phase-1 acceptance list.

## 14. Phase 3a: the swap transition (built), and the in-place swap (designed)

### 14.1 What the transition does now

Feedback from the first test: the wheel showed all 100 characters, and the swap passed visibly through character
select ("Retrieving character list") and then a loading screen.

- **Wheel:** it shows the character select's favorites (`WTF\Esteria\ECS-<account>-<realm>.txt`, field `f=`,
  read by `EsteriaSwap_GetFavorites`; the file name is matched without regard to case). Only favorites are shown.
- **Rise:** picking a character hides the HUD and lifts the camera 250 yd over 1.2 s. The request is sent at the top.
  `WorldScript::OnCameraUpdated` is a new hook, called right after `CGCamera::UpdateCallback`. The offset is undone
  after the frame is drawn (`RenderScript::OnWorldRendered`), so the camera's own smoothing never sees it. Over the
  last 0.4 s the addon fades in a full-screen sky built by `build_shell_mpq.py`: `Interface\Esteria\SwapSkyBack.tga`
  under `SwapSkyFront.tga`, each showing half its width and sliding at its own pace (240 s and 50 s periods). The
  cover draws the same layers on the same clock (`OsGetAsyncTimeMs`, `EsteriaSwap_SkyTime` in Lua). During
  synchronous loads the cover repaints only every 250 ms, so the drift steps there.
- **Cover:** `ClientDestroyGame` turns on the loading screen in swap mode (`LoadingScreenEnableSwapCover`). It shows
  the same sky with no bar and no tip, full screen. Character select and the real loading screen run behind it.
  It goes away when the world is loaded, as a loading screen does. It is also removed if the target is missing from
  the list, if `EnterWorld` refuses, or if the list request fails.
- **Arrive:** once the cover is gone and the server has sent the character, the camera starts 250 yd up and comes
  down over 1.6 s. The new interface holds the sky up from load until `PLAYER_ENTERING_WORLD`, then fades it out.
- **Refusal or no answer** (5 s after the climb): the camera comes back down in 0.8 s, the HUD returns and the error
  shows.

Later changes, measured with the per-step lines the extension writes to `Logs\Extensions.log` (`CharacterSwap:`):

- **Character select is not built behind the cover.** Its list is skipped. The target's model is still built, because
  the game takes it over at login (`m_activePlayerComponent`) and shows the character at once.
- **Quick map load.** Under the cover, `CMap::PrepareUpdate` waits once for the terrain in view, then for the doodads
  and buildings of the character's chunk (the streaming path's loops), instead of four full rounds. Map load went
  from 3.1–7.3 s to 2.1–2.7 s.
- **Settle.** After the cover goes, the camera holds in the sky behind the curtain until the file reads stop (0.25 s
  at least, 4 s at most), then the extension signals `ESTERIA_SWAP_ARRIVE` and the camera comes down. The curtain
  lifts on that signal, or after 10 s without it. This keeps the character and nearby buildings from popping in.
- **Shorter climb and descent:** 0.9 s and 1.3 s.
- **Cover redraws stay at the stock 4 a second.** 30 a second was tried and reverted while a crash was being chased.
- **Vulkan upload guard.** `CGxDeviceVulkan::ITexUpload` skips a texture whose texels end before its region, and logs
  it to `Logs\vulkan.log`, instead of crashing. The crash was first seen during swaps. The texture behind it is not
  identified yet.

A swap now takes about 12–13 s: climb 0.9 s, rebuilding the client 6.5 s, settling 3–4 s, descent 1.3 s.

### 14.2 What a swap with no load needs (not built)

Facts found in the client:

- `Packet_SMSG_LOGIN_VERIFY_WORLD` is also registered in the world (`ClientInitializeGame`). On the current map it
  does nothing. On another map it runs `LoadNewWorld`, which rebuilds the object manager and the map without leaving
  the game.
- The active player is whichever object arrives with create flag `0x1` (`ObjectMgrClient.cpp`,
  `s_curMgr->m_playerGuid = guid`), so a new character can replace the old one.
- `CGGameUI::Idle` already reloads the interface in place (`m_reloadUIRequested`: `Shutdown` +
  `InitializeFrameXML`). That is what loads per-character SavedVariables.
- The glue enter-world path calls `ClientInitializeGame` from the character list's map and position before it sends
  `CMSG_PLAYER_LOGIN`. So the current flow needs the list, and an in-place swap must not.

An in-place swap:

1. **Server:** `LogoutPlayer` variant that does not send `SMSG_LOGOUT_COMPLETE`. Then build `LoginQueryHolder` for the
   target on the same session and run `HandlePlayerLoginFromDB` (`LOGIN_VERIFY_WORLD` plus the normal burst). The
   save-order lock in §6.3 becomes necessary here, because the login no longer waits for a client round trip.
2. **Client, game state:** on a swap's `LOGIN_VERIFY_WORLD`, always run the object-manager half of `LoadNewWorld`,
   even on the same map. Skip `World::UnloadMap`/`LoadMap` when the map is the same. Then reset the per-character
   state that `ClientDestroyGame`/`ClientInitializeGame` normally rebuild: spellbook and cooldowns, action bars, loot,
   trade, party, mirror timers and account data (per-character macros and bindings).
3. **Client, interface:** run the in-place UI reload under the sky curtain instead of a loading screen. Flush the old
   character's SavedVariables first, and point the per-character SavedVariables path at the new name.
4. **Same map:** the descent can start at once. **Other map:** terrain loads under the curtain. That is the only real
   wait left.

Risk: step 2 is where stale per-character state in the client could leak between characters. It needs a careful
audit of every system `ClientDestroyGame` shuts down. Estimate: several days, and it should be done together with
phase 2 (shadows), which changes the same server path.
