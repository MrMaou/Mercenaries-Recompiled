-- Retail script API stubs; TEST_* globals let the test inspect effects.
TEST_scribble = { current_allies_mission = 3, current_china_mission = 1, SkipTo_Map = 'sw',
  card_clubs_2 = 'killed', card_clubs_14 = 'captured', card_diamonds_5 = 'captured' }
stateKilled, stateCaptured = 'killed', 'captured'
CARD_RANK_MIN, CARD_RANK_MAX = 2, 14
suit_sequence = { 'clubs', 'diamonds', 'hearts', 'spades' }
function gameflow_GetCardScribbleMemoryVariableName(suit, rank) return 'card_' .. suit .. '_' .. rank end
function Utility_ReadNumberFromScribbleMemory(k) return TEST_scribble[k] end
function Utility_ReadStringFromScribbleMemory(k) return TEST_scribble[k] end
TEST_scribble_writable = true
function Utility_WriteNumberToScribbleMemory(k, v) if TEST_scribble_writable then TEST_scribble[k] = v end end
-- A save is the scribble memory plus the money; loading restores both.
function TEST_save() local copy = {} for k, v in pairs(TEST_scribble) do copy[k] = v end return {scribble = copy, money = TEST_money} end
function TEST_load(save) TEST_scribble = {} for k, v in pairs(save.scribble) do TEST_scribble[k] = v end TEST_money = save.money end
TEST_money = 1000
function Player_GetMoney() return TEST_money end
function Player_SetMoney(v) TEST_money = v end
TEST_floors = {}
function Faction_SetMinimumRelation(f, v) TEST_floors[f] = v end
TEST_unlocked = {}
function Shop_UnlockItem(t) table.insert(TEST_unlocked, t) end
TEST_support = {}
function Support_AddItem(t, n) table.insert(TEST_support, t) end
TEST_hud = {}
function Ui_PrintHudMessage(m) table.insert(TEST_hud, m) end
tMafiaShop = { { template = 'template_support_deliverH2' } }
function gameflow_GetIntelTotal(ch) return 0 end
function AttemptFactionMoodClamp() end
TEST_ace = 0
function gameflow_AttemptAceMissionUnlock() TEST_ace = TEST_ace + 1 end
function TEST_dump(t) local o = {} for k, v in pairs(t) do table.insert(o, tostring(k) .. '=' .. tostring(v)) end table.sort(o) return table.concat(o, ',') end
