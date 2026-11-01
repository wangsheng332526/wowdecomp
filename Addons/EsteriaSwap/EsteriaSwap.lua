-- Character swap wheel. Hold the bound key, point at a character and release; or /swap and click one.
-- The CharacterSwap client extension does the rest: the server logs this character out and the client enters
-- the world as the chosen one without stopping at character select.

BINDING_HEADER_ESTERIASWAP = "Esteria"
BINDING_NAME_ESTERIASWAP_WHEEL = "Character swap wheel (hold)"

if not EsteriaSwap_Request then
    function EsteriaSwap_Wheel() end
    return
end

local CLASS_TOKENS = {
    "WARRIOR", "PALADIN", "HUNTER", "ROGUE", "PRIEST", "DEATHKNIGHT", "SHAMAN", "MAGE", "WARLOCK", nil, "DRUID",
}

-- SMSG_ESTERIA_SWAP_RESULT result codes, and the detail of result 3 (also the roster's "unsafe").
local RESULTS = {
    [1] = "That is not your character.",
    [2] = "Character swap is on cooldown.",
    [4] = "That character is online elsewhere.",
    [5] = "Character swap is disabled on this realm.",
    [6] = "Character swap failed.",
}
local UNSAFE = {
    "You can't swap characters in combat.",
    "You can't swap characters while casting.",
    "You can't swap characters while dead.",
    "You can't swap characters in an instance or battleground.",
    "Leave the queue before swapping characters.",
    "You can't swap characters while moving, flying or on a vehicle.",
    "Finish your duel, trade or loot before swapping characters.",
    "You can't swap characters with a pending instance bind.",
    "You can't swap characters in a raid group.",
}

local function Error(text)
    UIErrorsFrame:AddMessage(text, 1, 0.1, 0.1)
end

---------------------------------------------------------------------------------------------------------------------
-- The curtain: a full-screen sky over the world, the same picture as the client's swap cover. It fades in as the
-- camera reaches the sky, so the cut to the cover cannot be seen, and fades out on arrival while the camera comes
-- down. It has no parent so it stays up while UIParent is hidden.
---------------------------------------------------------------------------------------------------------------------
local curtain = CreateFrame("Frame", "EsteriaSwapCurtain")
curtain:SetAllPoints(WorldFrame)
curtain:SetFrameStrata("FULLSCREEN_DIALOG")
-- Two cloud layers, each showing half its width and sliding back and forth at its own pace: the same layers, periods
-- and clock as the client's cover (LoadingScreen.cpp, s_swapSkyLayers).
local layers = {
    { file = "Interface\\Esteria\\SwapSkyBack.tga", period = 240, phase = 0, level = "BACKGROUND" },
    { file = "Interface\\Esteria\\SwapSkyFront.tga", period = 50, phase = 1, level = "BORDER" },
}
for _, layer in ipairs(layers) do
    layer.texture = curtain:CreateTexture(nil, layer.level)
    layer.texture:SetAllPoints()
    layer.texture:SetTexture(layer.file)
end
curtain:Hide()

local function Drift()
    local time = EsteriaSwap_SkyTime()
    for _, layer in ipairs(layers) do
        local u = 0.25 + 0.25 * math.sin(time * 2 * math.pi / layer.period + layer.phase)
        layer.texture:SetTexCoord(u, u + 0.5, 0, 1)
    end
end

local fade, deadline, liftBy

local function Fade(from, to, delay, length, done)
    fade = { from = from, to = to, start = GetTime() + delay, length = length, done = done }
    curtain:SetAlpha(from)
    curtain:Show()
end

-- Arrived: the world has streamed in behind the curtain (ESTERIA_SWAP_ARRIVE) and the camera comes down.
local function Lift()
    liftBy = nil
    local _, descent = EsteriaSwap_IsArriving()
    Fade(curtain:GetAlpha(), 0, 0, descent * 0.6, function() curtain:Hide() end)
end

-- Refused, or no answer: the client brings the camera back down; bring the interface back with it.
local function Restore()
    deadline = nil
    if not UIParent:IsShown() then UIParent:Show() end
    Fade(curtain:GetAlpha(), 0, 0, 0.3, function() curtain:Hide() end)
end

curtain:SetScript("OnUpdate", function(self)
    Drift()
    if deadline and GetTime() > deadline then
        Restore()
    end
    if liftBy and GetTime() > liftBy then
        Lift()
    end
    if not fade then return end
    local t = (GetTime() - fade.start) / fade.length
    if t >= 1 then
        local done = fade.done
        self:SetAlpha(fade.to)
        fade = nil
        if done then done() end
    else
        self:SetAlpha(fade.from + (fade.to - fade.from) * math.max(t, 0))
    end
end)

---------------------------------------------------------------------------------------------------------------------
-- The wheel: EasyController's radial art (Tools/EsteriaShell/easycontroller, MIT). There is one background per number
-- of sections (1 to 8); the highlight and the veil are drawn for section 1 (12 o'clock) on the same 512 canvas and
-- turned about the wheel's centre onto section k. Sizes are the 512 art's, scaled to SIZE.
---------------------------------------------------------------------------------------------------------------------
local TEX = "Interface\\AddOns\\EsteriaSwap\\Textures\\"
local SIZE = 440
local S = SIZE / 512
local PER_PAGE = 8
local ICON_RADIUS, SLOT, HUB = 144 * S, 64 * S, 124 * S
local INNER, OUTER = 58 * S, 250 * S     -- the cursor aims between the hub and just past the rim
local LABEL_GAP = 52 * S
-- This client's SetRotation shows the image at 1/sqrt(2) of the texture's size: the overlays are sized up to match.
local OVERLAY = SIZE * math.sqrt(2)

local wheel = CreateFrame("Frame", "EsteriaSwapWheel", UIParent)
wheel:SetSize(SIZE, SIZE)
wheel:SetPoint("CENTER")
wheel:SetFrameStrata("DIALOG")
wheel:EnableMouse(true)
wheel:EnableMouseWheel(true)
wheel:Hide()
tinsert(UISpecialFrames, "EsteriaSwapWheel")

local background = wheel:CreateTexture(nil, "BACKGROUND")
background:SetAllPoints()

local function Overlay(sublevel)
    local texture = wheel:CreateTexture(nil, "ARTWORK", nil, sublevel)
    texture:SetSize(OVERLAY, OVERLAY)
    texture:SetPoint("CENTER")
    texture:Hide()
    return texture
end

-- Section i of n: its angle clockwise from 12 o'clock, and its direction (y up)
local function Angle(i, n) return (i - 1) * 2 * math.pi / n end
local function Direction(i, n)
    local a = Angle(i, n)
    return math.sin(a), math.cos(a)
end

local function Place(texture, kind, i, n)
    texture:SetTexture(TEX .. "wheel_" .. kind .. "_" .. n)
    texture:SetRotation(-Angle(i, n))
    texture:Show()
end

local veils, slots = {}, {}
for i = 1, PER_PAGE do
    veils[i] = Overlay(1)
end
local highlight = Overlay(2)

for i = 1, PER_PAGE do
    local slot = CreateFrame("Frame", nil, wheel)
    slot:SetSize(SLOT, SLOT)
    slot.disc = slot:CreateTexture(nil, "BACKGROUND")
    slot.disc:SetAllPoints()
    slot.disc:SetTexture(TEX .. "ck_slot")
    slot.icon = slot:CreateTexture(nil, "ARTWORK")
    slot.icon:SetPoint("TOPLEFT", SLOT * 0.1, -SLOT * 0.1)
    slot.icon:SetPoint("BOTTOMRIGHT", -SLOT * 0.1, SLOT * 0.1)
    slot.glow = slot:CreateTexture(nil, "OVERLAY")
    slot.glow:SetPoint("CENTER")
    slot.glow:SetSize(SLOT * 1.2, SLOT * 1.2)
    slot.glow:SetTexture(TEX .. "ck_slot_glow")
    slot.glow:Hide()
    slot.label = slot:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    slot.label:SetWidth(110 * S)
    slot:Hide()
    slots[i] = slot
end

-- The hub: the page, or the time left on the cooldown
local hub = CreateFrame("Frame", nil, wheel)
hub:SetSize(HUB, HUB)
hub:SetPoint("CENTER")
hub:SetFrameLevel(wheel:GetFrameLevel() + 10)
local hubArt = hub:CreateTexture(nil, "ARTWORK")
hubArt:SetAllPoints()
hubArt:SetTexture(TEX .. "ck_hub")
local hubText = hub:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
hubText:SetPoint("CENTER", 0, 4)
local hubHint = hub:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
hubHint:SetPoint("TOP", hubText, "BOTTOM", 0, -2)

-- Under the wheel: the aimed character, or why the wheel can't be used
local banner = wheel:CreateTexture(nil, "BACKGROUND")
banner:SetPoint("TOP", wheel, "BOTTOM", 0, 4)
banner:SetSize(340, 56)
banner:SetTexture(0, 0, 0, 0.55)
local bannerName = wheel:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
bannerName:SetPoint("TOP", banner, "TOP", 0, -10)
local bannerInfo = wheel:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
bannerInfo:SetPoint("TOP", bannerName, "BOTTOM", 0, -4)

local favorites, entries, page, aimed = {}, {}, 1, nil
local cooldownUntil = 0

local function Pages()
    return math.max(1, math.ceil(#entries / PER_PAGE))
end

local function PageEntries()
    local list = {}
    for i = (page - 1) * PER_PAGE + 1, math.min(#entries, page * PER_PAGE) do
        list[#list + 1] = entries[i]
    end
    return list
end

-- Why a swap would be refused now, as far as the client knows (the server checks again)
local function Blocked()
    local unsafe = EsteriaSwap_GetRoster().unsafe
    if unsafe > 0 then
        return UNSAFE[unsafe] or RESULTS[6]
    elseif GetTime() < cooldownUntil then
        return RESULTS[2]
    end
end

-- The character select's portrait key for a character (ECS_Schema S.GetRace): its race's override, else the first
-- known token inside its race's file name
local function ArtKey(character)
    local override = EsteriaSwapRaces.RaceOverride[character.race]
    if override then
        return override.artKey
    end
    local key = string.gsub(string.gsub(string.upper(character.raceFile or ""), "%.%a+$", ""), "[^%w]", "")
    for _, entry in ipairs(EsteriaSwapRaces.RaceByModelKey) do
        if string.find(key, entry[1], 1, true) then
            return entry[2].artKey
        end
    end
end

-- The same portrait as on the character select (ECS_Schema S.GetPortrait): ECS's round copy, else the create screen's
-- plate made round, else the class icon
local function SetPortrait(texture, character)
    local artKey = ArtKey(character)
    local sex = character.gender == 1 and "Female" or "Male"
    texture:SetTexCoord(0, 1, 0, 1)
    if artKey and EsteriaSwapRaces.PortraitArtKeys[artKey]
        and texture:SetTexture("Interface\\Glues\\CharacterSelect\\ECS-Portrait-" .. artKey .. sex) then
        return
    end
    local plate = artKey and "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-" .. artKey .. sex
    if plate and texture:SetTexture(plate) then
        SetPortraitToTexture(texture, plate)
        return
    end
    local coords = CLASS_ICON_TCOORDS[CLASS_TOKENS[character.class] or ""]
    if coords then
        texture:SetTexture("Interface\\TargetingFrame\\UI-Classes-Circles")
        texture:SetTexCoord(unpack(coords))
    else
        SetPortraitToTexture(texture, "Interface\\Icons\\INV_Misc_QuestionMark")
    end
end

-- The banner and the hub, for the aimed section (or none)
local function Describe()
    local list = PageEntries()
    local character = aimed and list[aimed]
    local blocked = Blocked()
    if character then
        bannerName:SetText(character.name)
        bannerInfo:SetText(string.format("Level %d  -  %s%s", character.level, character.zoneName,
            character.dead and "  |cffff2020(dead)|r" or ""))
    elseif #entries == 0 then
        bannerName:SetText("No favorites")
        bannerInfo:SetText("Mark characters as favorites on character select.")
    else
        bannerName:SetText("Swap character")
        bannerInfo:SetText(Pages() > 1 and "Point at a character. Mouse wheel: more." or "Point at a character.")
    end
    if blocked then
        bannerInfo:SetText("|cffff2020" .. blocked .. "|r")
    end

    if GetTime() < cooldownUntil then
        hubText:SetText(math.ceil(cooldownUntil - GetTime()))
        hubHint:SetText("cooldown")
    elseif Pages() > 1 then
        hubText:SetText(page .. "/" .. Pages())
        hubHint:SetText("")
    else
        hubText:SetText("")
        hubHint:SetText("")
    end
end

local function Aim(i)
    if i == aimed then return end
    aimed = i
    local n = #PageEntries()
    for k, slot in ipairs(slots) do
        if k == i then slot.glow:Show() else slot.glow:Hide() end
    end
    if i then
        Place(highlight, "sel", i, n)
    else
        highlight:Hide()
    end
    Describe()
end

local function Layout()
    local list = PageEntries()
    local n = #list
    local blocked = Blocked() ~= nil
    background:SetTexture(TEX .. "wheel_bg_" .. math.max(n, 1))
    for i, slot in ipairs(slots) do
        local character = list[i]
        if character then
            local dx, dy = Direction(i, n)
            slot:ClearAllPoints()
            slot:SetPoint("CENTER", wheel, "CENTER", ICON_RADIUS * dx, ICON_RADIUS * dy)
            SetPortrait(slot.icon, character)
            slot.icon:SetDesaturated(blocked and 1 or nil)
            -- The name on the outer side of the slot, as EasyController places it: beside it on the sides, above or
            -- below at the top and bottom, out on the diagonals.
            local ox, oy
            if math.abs(dx) > 0.8 then
                ox, oy = (dx > 0 and 1 or -1) * (LABEL_GAP + 14 * S), 0
            elseif math.abs(dx) < 0.3 then
                ox, oy = 0, (dy > 0 and 1 or -1) * LABEL_GAP
            else
                ox, oy = (dx > 0 and 1 or -1) * 30 * S, (dy > 0 and 1 or -1) * 45 * S
            end
            slot.label:ClearAllPoints()
            slot.label:SetPoint("CENTER", slot, "CENTER", ox, oy)
            slot.label:SetText(character.name)
            if blocked then slot.label:SetTextColor(0.5, 0.5, 0.5) else slot.label:SetTextColor(1, 0.82, 0) end
            slot:Show()
        else
            slot:Hide()
        end
        if character and blocked then
            Place(veils[i], "off", i, n)
        else
            veils[i]:Hide()
        end
    end
    aimed = false     -- not nil: Aim redraws even when nothing is aimed
    Aim(nil)
end

-- The section under the cursor: between the hub and just past the rim, the nearest section's direction
local function Aimed()
    local n = #PageEntries()
    if n == 0 then return nil end
    local x, y = GetCursorPosition()
    local scale = wheel:GetEffectiveScale()
    local cx, cy = wheel:GetCenter()
    local dx, dy = x / scale - cx, y / scale - cy
    local distance = math.sqrt(dx * dx + dy * dy)
    if distance < INNER or distance > OUTER then return nil end
    local fromNorth = (90 - math.deg(math.atan2(dy, dx))) % 360
    return math.floor((fromNorth + 180 / n) / (360 / n)) % n + 1
end

local function Close(commit)
    -- The server re-checks everything and answers with the reason in ESTERIA_SWAP_RESULT.
    local character = commit and aimed and PageEntries()[aimed]
    local rise = character and EsteriaSwap_Request(character.guid)
    aimed = nil
    wheel:Hide()
    if rise then
        if not InCombatLockdown() then UIParent:Hide() end
        Fade(0, 1, rise - 0.4, 0.4)
        -- The client gives up waiting 5 s after the climb.
        deadline = GetTime() + rise + 5.5
    end
end

-- Only the characters marked as favorites on the character select, in the roster's order.
local function Collect()
    entries = {}
    for _, character in ipairs(EsteriaSwap_GetRoster().characters) do
        if not character.active and favorites[string.lower(character.name)] then
            entries[#entries + 1] = character
        end
    end
    page = math.min(page, Pages())
    Layout()
end

local shownCooldown
wheel:SetScript("OnUpdate", function()
    Aim(Aimed())
    -- The hub's countdown, once a second
    local left = GetTime() < cooldownUntil and math.ceil(cooldownUntil - GetTime()) or 0
    if left ~= shownCooldown then
        shownCooldown = left
        if left == 0 then Layout() else Describe() end
    end
end)
wheel:SetScript("OnMouseUp", function(_, button)
    Close(button == "LeftButton")
end)
wheel:SetScript("OnMouseWheel", function(_, delta)
    local pages = Pages()
    if pages > 1 then
        page = (page - 1 - delta) % pages + 1
        Layout()
    end
end)

local function Open()
    favorites = EsteriaSwap_GetFavorites()
    page = 1
    wheel:Show()
    Collect()
    EsteriaSwap_RequestRoster()
end

function EsteriaSwap_Wheel(keystate)
    if keystate == "down" then
        Open()
    else
        Close(true)
    end
end

SLASH_ESTERIASWAP1 = "/swap"
SlashCmdList.ESTERIASWAP = function()
    if wheel:IsShown() then Close(false) else Open() end
end

-- Arriving from a swap: this interface was loaded behind the cover; keep the sky up while the world streams in, until
-- the client says it is coming down (or, should that never come, a while after the client's own 4 s limit).
if EsteriaSwap_IsArriving() then
    curtain:SetAlpha(1)
    curtain:Show()
    liftBy = GetTime() + 10
end

wheel:RegisterEvent("ESTERIA_SWAP_ROSTER")
wheel:RegisterEvent("ESTERIA_SWAP_RESULT")
wheel:RegisterEvent("ESTERIA_SWAP_ARRIVE")
wheel:SetScript("OnEvent", function(self, event, guid, result, detail, cooldownMs)
    if event == "ESTERIA_SWAP_ARRIVE" then
        Lift()
    elseif event == "ESTERIA_SWAP_ROSTER" then
        local cooldown = EsteriaSwap_GetRoster().cooldownMs
        cooldownUntil = cooldown > 0 and GetTime() + cooldown / 1000 or 0
        if wheel:IsShown() then Collect() end
    else
        cooldownUntil = cooldownMs > 0 and GetTime() + cooldownMs / 1000 or 0
        if result == 0 then
            deadline = nil
        else
            Restore()
        end
        if result == 3 then
            Error(UNSAFE[detail] or RESULTS[6])
        elseif result ~= 0 then
            Error(RESULTS[result] or RESULTS[6])
        end
    end
end)
