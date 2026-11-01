# CharacterSwap

Client side of the Esteria character hot-swap, phase 1 (`docs/character-hot-swap-plan.md`). The wheel UI is the
`Addons/EsteriaSwap` addon, shipped in the EsteriaShell MPQ. The server side is
`EsteriaWoW/src/server/game/Handlers/CharacterSwapHandler.cpp`, enabled with `Esteria.Swap.Enable = 1`.

An accepted swap is an instant logout. `SMSG_ESTERIA_SWAP_RESULT` (ok) stores the target in
`CCharacterSelection::s_autoEnterGuid`. The server's `SMSG_LOGOUT_COMPLETE` returns the client to character select.
When the next character list arrives, `CCharacterSelection::UpdateCharacterList` selects the target and calls
`CGlueMgr::EnterWorld`. The stored target is used once. A disconnect clears it.

There is no handshake, so update the client and the server together.

## Wire protocol

Little-endian integers, NUL-terminated UTF-8 strings.

| Opcode | Dir | Payload |
|---|---|---|
| `0x527` `CMSG_ESTERIA_SWAP_REQUEST` | C→S | guid(u64) |
| `0x528` `SMSG_ESTERIA_SWAP_RESULT` | S→C | guid(u64) result(u8) detail(u8) cooldownMs(u32) |
| `0x529` `CMSG_ESTERIA_SWAP_ROSTER_REQUEST` | C→S | none |
| `0x52A` `SMSG_ESTERIA_SWAP_ROSTER` | S→C | unsafe(u8) cooldownMs(u32) count(u8), then per character: guid(u64) name race(u8) class(u8) gender(u8) level(u8) map(u32) zone(u32) zoneName flags(u8: 1 active, 2 dead) |

Results: 0 ok, 1 not your character, 2 cooldown, 3 unsafe (reason in `detail`), 4 online on another account,
5 disabled, 6 internal error. Unsafe reasons, also used by the roster's `unsafe` byte: 1 combat, 2 casting,
3 dead, 4 instance or battleground, 5 queued, 6 teleport, taxi, vehicle or falling, 7 duel, trade or loot,
8 pending bind, 9 raid group.

## Lua

- `EsteriaSwap_RequestRoster()`: asks for `SMSG_ESTERIA_SWAP_ROSTER`.
- `EsteriaSwap_GetRoster()`: `{ unsafe, cooldownMs, characters = { { guid, name, race, class, gender, level, map, zone, zoneName, active, dead } } }`.
- `EsteriaSwap_Request(guid)`: `guid` is the `"0x..."` string from the roster.
- Events: `ESTERIA_SWAP_ROSTER`; `ESTERIA_SWAP_RESULT(guid, result, detail, cooldownMs)`.
