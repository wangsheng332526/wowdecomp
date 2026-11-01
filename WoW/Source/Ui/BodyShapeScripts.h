#ifndef WOW_UI_BODY_SHAPE_SCRIPTS_H
#define WOW_UI_BODY_SHAPE_SCRIPTS_H

// Lua entry points for the body sliders. Included by both script tables (in-game UI and glue/creator), so each
// file registers the same names: SetBodyShape, GetBodyShape, GetBodyShapeNames, GetBodyShapeSupport,
// GetBodyShapeRange, ResetBodyShape, BodyShapeRandomize, BodyShapeProbe, BodyShapeBoneInfo, BodyShapeRegion,
// BodyShapeMeshStatus, BodyShapeExport, BodyShapeImport, BodyShapeCopy, BodyShapePaste, GetBodyShapePresets, SaveBodyShapePreset,
// DeleteBodyShapePreset.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Retail/BodyShape.h"
#include "Engine/Source/Retail/BodyShapeMesh.h"
#include "Engine/Source/Os/OsClipboard.h"

// Body sliders. Values run -1..1; 0 leaves the model untouched. They edit the creator preview or the local player.
static int32_t Script_SetBodyShape(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isnumber(L, 2)) {
        luaL_error(L, "Usage: SetBodyShape(\"name\", value)");
        return 0;
    }

    lua_pushboolean(L, BodyShape::Set(lua_tolstring(L, 1, nullptr), static_cast<float>(lua_tonumber(L, 2))));
    return 1;
}

static int32_t Script_GetBodyShape(lua_State* L) {
    if (!lua_isstring(L, 1)) {
        luaL_error(L, "Usage: GetBodyShape(\"name\")");
        return 0;
    }

    lua_pushnumber(L, BodyShape::Get(lua_tolstring(L, 1, nullptr)));
    return 1;
}

// GetBodyShapeNames(["body" | "face"]) -> slider names, optionally one group only.
static int32_t Script_GetBodyShapeNames(lua_State* L) {
    const char* group = lua_isstring(L, 1) ? lua_tolstring(L, 1, nullptr) : nullptr;
    int32_t count = 0;

    for (const BodyShape::Slider& slider : BodyShape::Sliders()) {
        if (group && (slider.group == BodyShape::FaceGroup) != (strcmp(group, "face") == 0)) {
            continue;
        }

        lua_pushstring(L, slider.name);
        ++count;
    }

    return count;
}

// BodyShapeRegion("Brow") -> the model bone indices that face region moves (top-most bones only).
static int32_t Script_BodyShapeRegion(lua_State* L) {
    if (!lua_isstring(L, 1)) {
        return 0;
    }

    CM2Model* model = BodyShape::Edited().model;

    if (!model) {
        return 0;
    }

    BodyShape::State& state = BodyShape::For(model);
    auto it = state.regions.find(lua_tolstring(L, 1, nullptr));

    if (it == state.regions.end()) {
        return 0;
    }

    for (const BodyShape::RegionBone& bone : it->second.bones) {
        lua_pushnumber(L, bone.index);
    }

    return static_cast<int32_t>(it->second.bones.size());
}

// GetBodyShapeSupport("name") -> true / false, or nil while no model has loaded yet.
static int32_t Script_GetBodyShapeSupport(lua_State* L) {
    int32_t support = lua_isstring(L, 1) ? BodyShape::Supported(lua_tolstring(L, 1, nullptr)) : -1;

    if (support < 0) {
        lua_pushnil(L);
    } else {
        lua_pushboolean(L, support);
    }

    return 1;
}

// GetBodyShapeRange("name") -> min, max.
static int32_t Script_GetBodyShapeRange(lua_State* L) {
    int32_t found = lua_isstring(L, 1) ? BodyShape::FindSlider(lua_tolstring(L, 1, nullptr)) : -1;

    if (found < 0) {
        return 0;
    }

    lua_pushnumber(L, BodyShape::Sliders()[found].minValue);
    lua_pushnumber(L, 1.0);
    return 2;
}

// BodyShapeMeshStatus() -> searched, profileName, vertexCount, active. For diagnosing the breast/butt sliders.
static int32_t Script_BodyShapeMeshStatus(lua_State* L) {
    CM2Model* model = BodyShape::Edited().model;
    BodyShape::State empty;
    BodyShape::State& state = model ? BodyShape::For(model) : empty;
    lua_pushboolean(L, state.meshSearched);
    lua_pushstring(L, BodyShapeMesh::Name(state.meshProfile));
    lua_pushnumber(L, static_cast<double>(state.bind.size() / 6));
    lua_pushboolean(L, model && model->m_bodyShapeVertices != nullptr);
    return 4;
}

static int32_t Script_ResetBodyShape(lua_State* L) {
    BodyShape::Reset();
    return 0;
}

// BodyShapeProbe(index, mirror, sx, sy, sz, tx, ty, tz): scale and move one raw bone (and its mirror partner).
// index < 0 turns the probe off.
static int32_t Script_BodyShapeProbe(lua_State* L) {
    if (!lua_isnumber(L, 1)) {
        luaL_error(L, "Usage: BodyShapeProbe(index, mirror, sx, sy, sz, tx, ty, tz)");
        return 0;
    }

    float scale[3] = { 1.0f, 1.0f, 1.0f };
    float move[3] = { 0.0f, 0.0f, 0.0f };

    for (int32_t i = 0; i < 3; ++i) {
        if (lua_isnumber(L, 3 + i)) {
            scale[i] = static_cast<float>(lua_tonumber(L, 3 + i));
        }
        if (lua_isnumber(L, 6 + i)) {
            move[i] = static_cast<float>(lua_tonumber(L, 6 + i));
        }
    }

    BodyShape::SetProbe(static_cast<int32_t>(lua_tonumber(L, 1)), lua_toboolean(L, 2) != 0, scale, move);
    return 0;
}

// BodyShapeBoneInfo(index) -> keyId, parentIndex, x, y, z; with no argument -> boneCount. Nothing past the end.
static int32_t Script_BodyShapeBoneInfo(lua_State* L) {
    CM2Model* model = BodyShape::Edited().model;

    if (!model || !model->IsLoaded()) {
        return 0;
    }

    if (!lua_isnumber(L, 1)) {
        lua_pushnumber(L, model->GetBoneCount());
        return 1;
    }

    int32_t key, parent;
    C3Vector pivot;

    if (!model->GetBoneInfoByIndex(static_cast<uint32_t>(lua_tonumber(L, 1)), key, parent, pivot)) {
        return 0;
    }

    lua_pushnumber(L, key);
    lua_pushnumber(L, parent);
    lua_pushnumber(L, pivot.x);
    lua_pushnumber(L, pivot.y);
    lua_pushnumber(L, pivot.z);
    return 5;
}

// BodyShapeLog("text"): appends a line to Logs\BodyShape.log, so the creator Lua can report what it sees.
static int32_t Script_BodyShapeLog(lua_State* L) {
    if (lua_isstring(L, 1)) {
        BodyShape::Log(lua_tolstring(L, 1, nullptr));
    }

    return 0;
}

// BodyShapeRandomize(): random values for every slider the current model supports.
static int32_t Script_BodyShapeRandomize(lua_State* L) {
    BodyShape::Randomize();
    return 0;
}

// BodyShapeExport() -> a short code players can paste to each other: "BS:" + hex of the wire payload (version byte,
// then one byte per slot), trailing default slots left off.
static int32_t Script_BodyShapeExport(lua_State* L) {
    uint8_t payload[CharacterBodyShape::PayloadSize];
    CharacterBodyShape::WritePayload(payload, BodyShape::ToWire(BodyShape::Local()));
    size_t used = CharacterBodyShape::PayloadSize;

    while (used > 1 && payload[used - 1] == 0) {
        --used;
    }

    std::string code = "BS:";
    char hex[3];

    for (size_t i = 0; i < used; ++i) {
        std::snprintf(hex, sizeof(hex), "%02x", payload[i]);
        code += hex;
    }

    lua_pushstring(L, code.c_str());
    return 1;
}

// BodyShapeImport("BS:...") -> true and the sliders take the code's values, or false for anything malformed.
static int32_t Script_BodyShapeImport(lua_State* L) {
    std::string code = lua_isstring(L, 1) ? lua_tolstring(L, 1, nullptr) : "";
    code.erase(0, code.find_first_not_of(" \t\r\n"));
    code.erase(code.find_last_not_of(" \t\r\n") + 1);

    uint8_t payload[CharacterBodyShape::PayloadSize] = {};
    bool ok = code.size() > 3 && (code[0] == 'B' || code[0] == 'b') && (code[1] == 'S' || code[1] == 's')
        && code[2] == ':' && code.size() % 2 == 1 && (code.size() - 3) / 2 <= CharacterBodyShape::PayloadSize;

    for (size_t i = 3; ok && i < code.size(); i += 2) {
        ok = std::isxdigit(static_cast<unsigned char>(code[i])) && std::isxdigit(static_cast<unsigned char>(code[i + 1]));
        payload[(i - 3) / 2] = static_cast<uint8_t>(std::strtoul(code.substr(i, 2).c_str(), nullptr, 16));
    }

    CharacterBodyShape::Values wire{};
    ok = ok && CharacterBodyShape::ReadPayload(payload, CharacterBodyShape::PayloadSize, wire);

    if (ok) {
        BodyShape::Local().v = BodyShape::FromWire(wire).v;
    }

    lua_pushboolean(L, ok);
    return 1;
}

// BodyShapeCopy("text") -> true when the text is on the system clipboard.
static int32_t Script_BodyShapeCopy(lua_State* L) {
    lua_pushboolean(L, lua_isstring(L, 1) && OsClipboardPutString(lua_tolstring(L, 1, nullptr)));
    return 1;
}

// BodyShapePaste() -> the clipboard's text, or nil.
static int32_t Script_BodyShapePaste(lua_State* L) {
    char* text = OsClipboardGetString();

    if (!text) {
        return 0;
    }

    lua_pushstring(L, text);
    OsClipboardFreeString(text);
    return 1;
}

// Body presets live in WTF\BodyShapePresets.txt, one "key<TAB>name<TAB>code" per line. The creator uses race:sex as key.
static std::vector<std::array<std::string, 3>> BodyShapeReadPresets() {
    std::vector<std::array<std::string, 3>> presets;
    std::ifstream file("WTF/BodyShapePresets.txt");
    std::string line;

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const size_t a = line.find('\t');
        const size_t b = a == std::string::npos ? a : line.find('\t', a + 1);

        if (b != std::string::npos) {
            presets.push_back({ line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1) });
        }
    }

    return presets;
}

static bool BodyShapeWritePresets(const std::vector<std::array<std::string, 3>>& presets) {
    std::ofstream file("WTF/BodyShapePresets.txt", std::ios::trunc);

    for (const auto& preset : presets) {
        file << preset[0] << '\t' << preset[1] << '\t' << preset[2] << '\n';
    }

    return static_cast<bool>(file);
}

// Tabs and line breaks would break the file format.
static std::string BodyShapePresetField(lua_State* L, int32_t index) {
    std::string text = lua_isstring(L, index) ? lua_tolstring(L, index, nullptr) : "";

    for (char& c : text) {
        if (c == '\t' || c == '\r' || c == '\n') {
            c = ' ';
        }
    }

    return text;
}

// GetBodyShapePresets("key") -> name1, code1, name2, code2, ...
static int32_t Script_GetBodyShapePresets(lua_State* L) {
    const std::string key = BodyShapePresetField(L, 1);
    int32_t count = 0;

    for (const auto& preset : BodyShapeReadPresets()) {
        if (preset[0] == key && lua_checkstack(L, 2)) {
            lua_pushstring(L, preset[1].c_str());
            lua_pushstring(L, preset[2].c_str());
            count += 2;
        }
    }

    return count;
}

// SaveBodyShapePreset("key", "name", "code") -> true when written; replaces a preset of the same key and name.
static int32_t Script_SaveBodyShapePreset(lua_State* L) {
    const std::string key = BodyShapePresetField(L, 1);
    const std::string name = BodyShapePresetField(L, 2);
    const std::string code = BodyShapePresetField(L, 3);

    if (key.empty() || name.empty() || code.empty()) {
        lua_pushboolean(L, false);
        return 1;
    }

    auto presets = BodyShapeReadPresets();
    bool replaced = false;

    for (auto& preset : presets) {
        if (preset[0] == key && preset[1] == name) {
            preset[2] = code;
            replaced = true;
        }
    }

    if (!replaced) {
        presets.push_back({ key, name, code });
    }

    lua_pushboolean(L, BodyShapeWritePresets(presets));
    return 1;
}

// DeleteBodyShapePreset("key", "name")
static int32_t Script_DeleteBodyShapePreset(lua_State* L) {
    const std::string key = BodyShapePresetField(L, 1);
    const std::string name = BodyShapePresetField(L, 2);
    auto presets = BodyShapeReadPresets();
    presets.erase(std::remove_if(presets.begin(), presets.end(),
        [&](const auto& preset) { return preset[0] == key && preset[1] == name; }), presets.end());
    BodyShapeWritePresets(presets);
    return 0;
}

#define BODY_SHAPE_SCRIPT_ENTRIES \
    { "SetBodyShape", &Script_SetBodyShape }, \
    { "GetBodyShape", &Script_GetBodyShape }, \
    { "GetBodyShapeNames", &Script_GetBodyShapeNames }, \
    { "ResetBodyShape", &Script_ResetBodyShape }, \
    { "BodyShapeRandomize", &Script_BodyShapeRandomize }, \
    { "GetBodyShapeSupport", &Script_GetBodyShapeSupport }, \
    { "BodyShapeRegion", &Script_BodyShapeRegion }, \
    { "GetBodyShapeRange", &Script_GetBodyShapeRange }, \
    { "BodyShapeMeshStatus", &Script_BodyShapeMeshStatus }, \
    { "BodyShapeProbe", &Script_BodyShapeProbe }, \
    { "BodyShapeBoneInfo", &Script_BodyShapeBoneInfo }, \
    { "BodyShapeLog", &Script_BodyShapeLog }, \
    { "BodyShapeExport", &Script_BodyShapeExport }, \
    { "BodyShapeImport", &Script_BodyShapeImport }, \
    { "BodyShapeCopy", &Script_BodyShapeCopy }, \
    { "BodyShapePaste", &Script_BodyShapePaste }, \
    { "GetBodyShapePresets", &Script_GetBodyShapePresets }, \
    { "SaveBodyShapePreset", &Script_SaveBodyShapePreset }, \
    { "DeleteBodyShapePreset", &Script_DeleteBodyShapePreset },

#endif
