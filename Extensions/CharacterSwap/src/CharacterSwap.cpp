#include "Engine/Source/Base/CDataStore.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/lua-5.1/src/lauxlib.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Services/AsyncFile.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/Extension/ExtensionMgr.h"
#include "WoW/Source/Glue/CharacterSelection.h"
#include "WoW/Source/LoadingScreen.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/ClientServices.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Character hot-swap (docs/character-hot-swap-plan.md). A swap runs in four steps:
//   rise:   the camera climbs above the character (RiseSec), then the request goes to the server;
//   wait:   the camera holds in the sky until SMSG_ESTERIA_SWAP_RESULT; a refusal brings it back down;
//   cover:  the server logs out, ClientDestroyGame covers the screen with the swap sky (LoadingScreenEnableSwapCover)
//           and the next character list enters the world as the target (CCharacterSelection::s_autoEnterGuid);
//   settle: once the new world is up the camera holds in the sky behind the addon's curtain while the rest of the map
//           streams in (the map load only waited for what the character stands on), until the file reads stop or
//           SettleMaxMs has passed;
//   arrive: the curtain fades (ESTERIA_SWAP_ARRIVE) and the camera comes down onto the new character.
// The EsteriaSwap addon fades a full-screen sky in at the end of the rise and out on arrival, so the hard cuts to and
// from the cover happen behind the same picture.

namespace {
constexpr uint32_t CMSG_ESTERIA_SWAP_REQUEST = 0x527;
constexpr uint32_t SMSG_ESTERIA_SWAP_RESULT = 0x528;
constexpr uint32_t CMSG_ESTERIA_SWAP_ROSTER_REQUEST = 0x529;
constexpr uint32_t SMSG_ESTERIA_SWAP_ROSTER = 0x52A;

struct Character {
    uint64_t guid = 0;
    std::string name, zoneName;
    uint8_t race = 0, classID = 0, gender = 0, level = 0, flags = 0;
    uint32_t map = 0, zone = 0;
};

struct Roster {
    uint8_t unsafe = 0;
    uint32_t cooldownMs = 0;
    std::vector<Character> characters;
};

Roster s_roster;

enum class Phase { None, Rise, Wait, Cover, Settle, Arrive, Return };
constexpr float RiseSec = 0.9f, DescendSec = 1.3f, ReturnSec = 0.8f, SkyHeight = 250.0f;
constexpr uint32_t SettleMinMs = 250, SettleMaxMs = 4000;
Phase s_phase = Phase::None;
float s_time = 0.0f;
uint32_t s_settleStart = 0;
bool s_leftWorld = false;     // this swap's logout has torn the old world down
uint64_t s_target = 0;

float Smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// How far above its normal place the camera is this frame.
float SkyOffset() {
    float rise = std::clamp(s_time / RiseSec, 0.0f, 1.0f);
    switch (s_phase) {
        case Phase::Rise: return SkyHeight * rise * rise;
        case Phase::Wait:
        case Phase::Cover:
        case Phase::Settle: return SkyHeight;
        case Phase::Arrive: return SkyHeight * (1.0f - Smooth(s_time / DescendSec));
        case Phase::Return: return SkyHeight * (1.0f - Smooth(s_time / ReturnSec));
        default: return 0.0f;
    }
}

bool SendRequest(uint64_t guid) {
    CDataStore payload;
    payload.Put(guid);
    payload.Finalize();
    return sExtensionMgr->SendClientMessage(CMSG_ESTERIA_SWAP_REQUEST, payload);
}

// Where a swap's time goes: one Extensions.log line per step, in milliseconds since the character was picked.
uint32_t s_pickedAt = 0;
void Mark(char const* step) {
    if (s_phase != Phase::None) sExtensionMgr->Log("CharacterSwap: %6u ms  %s", OsGetAsyncTimeMs() - s_pickedAt, step);
}

bool Read(CDataStore* message, std::vector<uint8_t>& bytes) {
    if (!message || message->m_read > message->m_size) return false;
    uint32_t size = message->m_size - message->m_read;
    if (size > (1u << 16) || !message->FetchRead(message->m_read, size)) return false;
    bytes.resize(size);
    message->GetData(bytes.data(), size);
    return true;
}

class Reader {
public:
    explicit Reader(std::vector<uint8_t> const& bytes) : m_bytes(bytes) { }
    template <typename T> bool Get(T& result) {
        if (sizeof(T) > m_bytes.size() - m_offset) return false;
        result = 0;
        for (size_t i = 0; i < sizeof(T); ++i) result |= T(m_bytes[m_offset++]) << (i * 8);
        return true;
    }
    bool String(std::string& result, size_t maximum) {
        size_t start = m_offset;
        while (m_offset < m_bytes.size() && m_bytes[m_offset] && m_offset - start <= maximum) ++m_offset;
        if (m_offset == m_bytes.size() || m_offset - start > maximum) return false;
        result.assign(reinterpret_cast<char const*>(m_bytes.data() + start), m_offset++ - start);
        return true;
    }
    bool Done() const { return m_offset == m_bytes.size(); }
private:
    std::vector<uint8_t> const& m_bytes;
    size_t m_offset = 0;
};

std::string GuidString(uint64_t guid) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%016llX", static_cast<unsigned long long>(guid));
    return text;
}

// guid(8) result(u8) detail(u8) cooldownMs(u32)
int32_t ReceiveResult(void*, NETMESSAGE, uint32_t, CDataStore* message) {
    std::vector<uint8_t> bytes;
    if (!Read(message, bytes)) return 0;
    Reader reader(bytes);
    uint64_t guid;
    uint8_t result, detail;
    uint32_t cooldownMs;
    if (!reader.Get(guid) || !reader.Get(result) || !reader.Get(detail) || !reader.Get(cooldownMs) || !reader.Done())
        return 0;
    // The SMSG_LOGOUT_COMPLETE that follows an accepted swap returns to the glue, which then enters as guid.
    if (result == 0) {
        CCharacterSelection::s_autoEnterGuid = { guid };
        s_phase = Phase::Cover;
        Mark("accepted");
    } else if (s_phase == Phase::Rise || s_phase == Phase::Wait) {
        s_phase = Phase::Return;
        s_time = 0.0f;
    }
    uint32_t event = sExtensionMgr->GetFrameScriptEventIndex("ESTERIA_SWAP_RESULT");
    if (event != UINT32_MAX) FrameScript_SignalEvent(event, "%s%d%d%d", GuidString(guid).c_str(), result, detail, cooldownMs);
    return 1;
}

// unsafe(u8) cooldownMs(u32) count(u8), then per character:
// guid(8) name race(u8) class(u8) gender(u8) level(u8) map(u32) zone(u32) zoneName flags(u8)
int32_t ReceiveRoster(void*, NETMESSAGE, uint32_t, CDataStore* message) {
    std::vector<uint8_t> bytes;
    if (!Read(message, bytes)) return 0;
    Reader reader(bytes);
    Roster next;
    uint8_t count;
    if (!reader.Get(next.unsafe) || !reader.Get(next.cooldownMs) || !reader.Get(count)) return 0;
    for (uint8_t i = 0; i < count; ++i) {
        Character c;
        if (!reader.Get(c.guid) || !reader.String(c.name, 48) || !reader.Get(c.race) || !reader.Get(c.classID)
            || !reader.Get(c.gender) || !reader.Get(c.level) || !reader.Get(c.map) || !reader.Get(c.zone)
            || !reader.String(c.zoneName, 256) || !reader.Get(c.flags)) return 0;
        next.characters.push_back(std::move(c));
    }
    if (!reader.Done()) return 0;
    s_roster = std::move(next);
    uint32_t event = sExtensionMgr->GetFrameScriptEventIndex("ESTERIA_SWAP_ROSTER");
    if (event != UINT32_MAX) FrameScript_SignalEvent(event, nullptr);
    return 1;
}

int32_t RequestRoster(lua_State* L) {
    lua_pushboolean(L, sExtensionMgr->SendClientMessage(CMSG_ESTERIA_SWAP_ROSTER_REQUEST, nullptr, 0));
    return 1;
}

int32_t Request(lua_State* L) {
    size_t size = 0;
    char const* text = lua_tolstring(L, 1, &size);
    if (text && size >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { text += 2; size -= 2; }
    uint64_t guid = 0;
    auto parsed = text ? std::from_chars(text, text + size, guid, 16) : std::from_chars_result{ nullptr, std::errc::invalid_argument };
    if (!text || parsed.ec != std::errc() || parsed.ptr != text + size || !guid)
        return luaL_error(L, "Usage: EsteriaSwap_Request(guid)");
    // Returns how long the camera climbs before the request is sent, or nil when a swap is already running.
    if (s_phase != Phase::None) {
        lua_pushnil(L);
        return 1;
    }
    s_target = guid;
    s_phase = Phase::Rise;
    s_time = 0.0f;
    s_leftWorld = false;
    s_pickedAt = OsGetAsyncTimeMs();
    lua_pushnumber(L, RiseSec);
    return 1;
}

// True while a swap is bringing a character in (cover, settling, then the descent); also returns the descent's length.
int32_t IsArriving(lua_State* L) {
    lua_pushboolean(L, s_phase == Phase::Cover || s_phase == Phase::Settle || s_phase == Phase::Arrive);
    lua_pushnumber(L, DescendSec);
    return 2;
}

// The clock the swap sky drifts by, shared with LoadingScreen's cover so the curtain and the cover line up.
int32_t SkyTime(lua_State* L) {
    lua_pushnumber(L, OsGetAsyncTimeMs() / 1000.0);
    return 1;
}

std::string PercentDecode(std::string const& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned value = 0;
        if (text[i] == '%' && i + 2 < text.size()
            && std::from_chars(text.data() + i + 1, text.data() + i + 3, value, 16).ec == std::errc()) {
            out += static_cast<char>(value);
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

// The character select's favorites (WTF\Esteria\ECS-<account>-<realm>.txt, field f=), as { [lowercase name] = true }.
int32_t GetFavorites(lua_State* L) {
    lua_newtable(L);
    // The glue names the file after the saved login name (as typed); the session holds it in another case.
    std::string account;
    for (char const* c = ClientServices::GetAccountName(); c && *c && account.size() < 64; ++c) {
        if (std::isalnum(static_cast<unsigned char>(*c)) || *c == '_' || *c == '-') {
            account += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        }
    }
    if (account.empty()) return 1;
    std::string const prefix = "ecs-" + account + "-";
    std::error_code error;
    std::filesystem::directory_iterator it(std::filesystem::path("WTF") / "Esteria", error), end;
    for (; !error && it != end; it.increment(error)) {
        std::string file = it->path().filename().string();
        std::transform(file.begin(), file.end(), file.begin(), [](unsigned char c) { return std::tolower(c); });
        if (file.compare(0, prefix.size(), prefix) != 0) continue;
        std::ifstream in(it->path(), std::ios::binary);
        std::string const text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        size_t start = text.find("|f=");
        if (start == std::string::npos) continue;
        start += 3;
        std::string const list = text.substr(start, text.find('|', start) - start);
        for (size_t at = 0; at < list.size();) {
            size_t comma = std::min(list.find(',', at), list.size());
            std::string const key = PercentDecode(list.substr(at, comma - at));
            size_t colon = key.find(':');
            std::string const name = colon == std::string::npos ? key : key.substr(colon + 1);
            if (!name.empty()) {
                lua_pushlstring(L, name.data(), name.size());
                lua_pushboolean(L, 1);
                lua_settable(L, -3);
            }
            at = comma + 1;
        }
    }
    return 1;
}

void NumberField(lua_State* L, char const* key, double value) {
    lua_pushnumber(L, value); lua_setfield(L, -2, key);
}
void BoolField(lua_State* L, char const* key, bool value) {
    lua_pushboolean(L, value); lua_setfield(L, -2, key);
}
void StringField(lua_State* L, char const* key, std::string const& value) {
    lua_pushlstring(L, value.data(), value.size()); lua_setfield(L, -2, key);
}

int32_t GetRoster(lua_State* L) {
    lua_createtable(L, 0, 3);
    NumberField(L, "unsafe", s_roster.unsafe);
    NumberField(L, "cooldownMs", s_roster.cooldownMs);
    lua_createtable(L, int(s_roster.characters.size()), 0);
    int i = 0;
    for (Character const& c : s_roster.characters) {
        lua_createtable(L, 0, 12);
        StringField(L, "guid", GuidString(c.guid)); StringField(L, "name", c.name);
        NumberField(L, "race", c.race); NumberField(L, "class", c.classID); NumberField(L, "gender", c.gender);
        NumberField(L, "level", c.level); NumberField(L, "map", c.map); NumberField(L, "zone", c.zone);
        StringField(L, "zoneName", c.zoneName);
        // ChrRaces' file string: what the character select's portrait lookup matches on (EsteriaSwap Races.lua)
        ChrRacesRec* race = g_chrRacesDB.GetRecord(c.race);
        StringField(L, "raceFile", race && race->m_clientFileString ? race->m_clientFileString : "");
        BoolField(L, "active", (c.flags & 1) != 0); BoolField(L, "dead", (c.flags & 2) != 0);
        lua_rawseti(L, -2, ++i);
    }
    lua_setfield(L, -2, "characters");
    return 1;
}

class Lifecycle : public NetScript {
public:
    Lifecycle() : NetScript("CharacterSwapLifecycle") { }
    // A dropped connection goes to the login screen; do not auto-enter on the next session's character list.
    void OnDisconnected(NetClient&) override {
        CCharacterSelection::s_autoEnterGuid = { 0 };
        s_roster = Roster{};
        s_phase = Phase::None;
    }
};

class Sequence : public WorldScript {
public:
    Sequence() : WorldScript("CharacterSwapSequence") { }

    void OnWorldUpdate(float elapsedSec) override {
        if (s_phase == Phase::None) return;
        if (s_phase == Phase::Cover) {
            // The new character's world is up once the cover is gone and the server has sent the character.
            // The accept can arrive a moment before the logout: only a world entered after the teardown counts.
            if (!s_leftWorld || LoadingScreenDrawing() || !ClntObjMgrGetActivePlayer()) return;
            s_phase = Phase::Settle;
            s_settleStart = OsGetAsyncTimeMs();
            Mark("cover gone, streaming the map in behind the curtain");
            return;
        }
        if (s_phase == Phase::Settle) {
            uint32_t waited = OsGetAsyncTimeMs() - s_settleStart;
            if (waited < SettleMinMs || (AsyncFileReadIsReading() && waited < SettleMaxMs)) return;
            s_phase = Phase::Arrive;
            s_time = 0.0f;
            Mark(waited >= SettleMaxMs ? "still streaming, coming down anyway" : "streamed in, coming down");
            uint32_t event = sExtensionMgr->GetFrameScriptEventIndex("ESTERIA_SWAP_ARRIVE");
            if (event != UINT32_MAX) FrameScript_SignalEvent(event, nullptr);
            return;
        }
        s_time += std::min(elapsedSec, 0.1f);
        if (s_phase == Phase::Rise && s_time >= RiseSec) {
            s_phase = SendRequest(s_target) ? Phase::Wait : Phase::Return;
            s_time = 0.0f;
            Mark("request sent");
        } else if (s_phase == Phase::Wait && s_time >= 5.0f) {
            // No answer from the server: come back down rather than hang in the sky.
            s_phase = Phase::Return;
            s_time = 0.0f;
        } else if ((s_phase == Phase::Arrive && s_time >= DescendSec) || (s_phase == Phase::Return && s_time >= ReturnSec)) {
            Mark("done");
            s_phase = Phase::None;
        }
    }

    void OnLogoutComplete() override {
        s_leftWorld = true;
        Mark("logged out, tearing the world down");
    }
    void OnMapLoaded(uint32_t, char const*) override { Mark("map loaded (login is sent next)"); }
    void OnLoginVerifyWorld(uint32_t, C3Vector const&, float) override { Mark("server sent the character"); }

    // The world (and its camera) may go before another frame is drawn.
    void OnMapUnload() override { m_camera = nullptr; }

    void OnCameraUpdated(CGCamera& camera) override {
        float height = SkyOffset();
        if (height <= 0.0f || camera.m_relativeTo) return;
        CGObject_C* player = ClntObjMgrObjectPtr(ClntObjMgrGetActivePlayer(), TYPEMASK_PLAYER, __FILE__, __LINE__);
        if (!player) return;

        m_camera = &camera;
        m_position = camera.m_position;
        m_facing = camera.m_facing;

        // Climb, and pull back a third as far, so the view tilts down onto the character rather than straight down.
        C3Vector focus = player->GetPosition();
        focus.z += 1.5f;
        C3Vector position = camera.m_position;
        float dx = position.x - focus.x, dy = position.y - focus.y;
        float flat = std::sqrt(dx * dx + dy * dy);
        if (flat < 0.01f) { dx = 1.0f; dy = 0.0f; flat = 1.0f; }
        position.x += dx / flat * height / 3.0f;
        position.y += dy / flat * height / 3.0f;
        position.z += height;
        camera.m_position = position;

        C3Vector forward(focus.x - position.x, focus.y - position.y, focus.z - position.z);
        float length = std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
        camera.SetFacing(C3Vector(forward.x / length, forward.y / length, forward.z / length));
    }

    // Put the camera back once the frame is drawn, so its own smoothing never sees the offset.
    void Restore() {
        if (!m_camera) return;
        m_camera->m_position = m_position;
        m_camera->m_facing = m_facing;
        m_camera = nullptr;
    }

private:
    CGCamera* m_camera = nullptr;
    C3Vector m_position;
    C33Matrix m_facing;
};

Sequence* s_sequence = nullptr;

// The interface's two Lua states coming up: the glue's ends the teardown, the game's starts the interface load
class Timing : public LuaScript {
public:
    Timing() : LuaScript("CharacterSwapTiming") { }
    void OnFrameScriptInitialized(lua_State*, FrameScriptContext context) override {
        Mark(context == FRAMESCRIPT_CONTEXT_GLUE ? "glue up, asking for the character list" : "loading the interface");
    }
};

class Restorer : public RenderScript {
public:
    Restorer() : RenderScript("CharacterSwapCameraRestore") { }
    void OnWorldRendered() override { s_sequence->Restore(); }
};
}

void AddCharacterSwapExtension() {
    sExtensionMgr->RegisterMessageHandler(SMSG_ESTERIA_SWAP_RESULT, ReceiveResult, nullptr);
    sExtensionMgr->RegisterMessageHandler(SMSG_ESTERIA_SWAP_ROSTER, ReceiveRoster, nullptr);
    sExtensionMgr->RegisterLuaFunction("EsteriaSwap_RequestRoster", RequestRoster, FRAMESCRIPT_CONTEXTS_GAME);
    sExtensionMgr->RegisterLuaFunction("EsteriaSwap_GetRoster", GetRoster, FRAMESCRIPT_CONTEXTS_GAME);
    sExtensionMgr->RegisterLuaFunction("EsteriaSwap_Request", Request, FRAMESCRIPT_CONTEXTS_GAME);
    sExtensionMgr->RegisterLuaFunction("EsteriaSwap_IsArriving", IsArriving, FRAMESCRIPT_CONTEXTS_GAME);
    sExtensionMgr->RegisterLuaFunction("EsteriaSwap_GetFavorites", GetFavorites, FRAMESCRIPT_CONTEXTS_GAME);
    sExtensionMgr->RegisterLuaFunction("EsteriaSwap_SkyTime", SkyTime, FRAMESCRIPT_CONTEXTS_GAME);
    sExtensionMgr->RegisterFrameScriptEvent("ESTERIA_SWAP_ROSTER", FRAMESCRIPT_CONTEXTS_GAME);
    sExtensionMgr->RegisterFrameScriptEvent("ESTERIA_SWAP_RESULT", FRAMESCRIPT_CONTEXTS_GAME);
    sExtensionMgr->RegisterFrameScriptEvent("ESTERIA_SWAP_ARRIVE", FRAMESCRIPT_CONTEXTS_GAME);
    new Lifecycle();
    s_sequence = new Sequence();
    new Restorer();
    new Timing();
}
