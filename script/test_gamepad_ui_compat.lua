-- Standalone contract test for the GAMMA UI overlay. Run with:
--   luajit script/test_gamepad_ui_compat.lua

local source = debug.getinfo(1, "S").source:sub(2)
local script_path = source:gsub("test_gamepad_ui_compat%.lua$", "../gamedata/scripts/gamepad_ui_compat.script")

local function window(focused)
	local result = { focused = focused or false }
	function result:SetGamepadFocusable(value) self.focusable = value end
	function result:SetGamepadRole(value) self.role = value end
	function result:GetGamepadRole() return self.role or 0 end
	function result:GetGamepadFocused() return self.focused end
	function result:GetGamepadFocusable() return self.focusable or false end
	return result
end

local function item(focused, shown, owner_container)
	local cell = window(focused)
	return {
		cell = cell,
		container = owner_container,
		shown = shown ~= false,
		IsShown = function(self) return self.shown end,
	}
end

local UICellContainer = {
	Callback = function(self, name, _, index)
		local owner = self.owner
		if self.disable_callback and self.disable_callback[name] then return false end
		if owner[name] then return owner[name](owner, self.ID, index) end
		if name == "On_CC_Mouse1_DB" then return false end
		return nil
	end,
	Update = function(self) self.updated = true end,
	AddItemManual = function(self, _, _, index)
		self.cell[index].shown = true
		return true
	end,
	RemoveItemManual = function(self, index)
		self.cell[index].shown = false
	end,
}

local function container(id, owner, focused, shown, cell_shown, options)
	options = options or {}
	local result = {
		ID = id,
		owner = owner,
		prof = window(false),
		cell = {},
		shown = shown ~= false,
		hovered = nil,
		selected = nil,
		disable_callback = options.disable_callback or {},
		disable_highlight = options.disable_highlight,
		callback_calls = {},
		IsShown = function(self) return self.shown end,
		On_Hover = function(self, index) self.hovered = index end,
		On_Select = function(self, index) self.selected = index end,
	}
	setmetatable(result, { __index = UICellContainer })
	result.cell[1] = item(focused, cell_shown, result)
	if options.override_callback then
		result.Callback = function(self, ...) return nil end
	end
	return result
end

local function dialog_class()
	return {
		SetGamepadRole = function(self, value) self.role = value end,
		GetGamepadRole = function(self) return self.role or 0 end,
		SetGamepadFocusable = function(self, value) self.focusable = value end,
		GetGamepadFocusable = function(self) return self.focusable or false end,
	}
end

local function owner_instance(class, fields)
	fields = fields or {}
	setmetatable(fields, { __index = class })
	return fields
end

local function check(initial)
	return {
		value = initial,
		GetCheck = function(self) return self.value end,
		SetCheck = function(self, value) self.value = value end,
	}
end

local UICellItem = { InitControls = function(self) self.cell = window(false) end }
local UIInventory = dialog_class()
UIInventory.Dispatch = function(_, command) return command == 99 end
UIInventory.sort_btn = { check(true), check(false), check(false) }
UIInventory.sort_calls = {}
UIInventory.On_Sort = function(self, index, ignore)
	self.sort_calls[#self.sort_calls + 1] = { index = index, ignore = ignore }
	if ignore then
		for i = 1, #self.sort_btn do self.sort_btn[i]:SetCheck(i == index) end
	end
end
UIInventory.On_CC_Mouse1 = function(self, bag, index)
	self.actions[#self.actions + 1] = { name = "On_CC_Mouse1", bag = bag, index = index }
end
UIInventory.On_CC_Mouse1_DB = function(self, bag, index)
	self.actions[#self.actions + 1] = { name = "On_CC_Mouse1_DB", bag = bag, index = index }
end
UIInventory.On_CC_Mouse2 = function(self, bag, index)
	self.actions[#self.actions + 1] = { name = "On_CC_Mouse2", bag = bag, index = index }
end

local UINewGame = dialog_class()
UINewGame.Dispatch = function(_, command) return command == 99 end
UINewGame.Update = function() end
UINewGame.On_CC_Mouse1 = function(self, bag, index)
	self.actions[#self.actions + 1] = { name = "On_CC_Mouse1", bag = bag, index = index }
end

local UIWorkshop = dialog_class()
UIWorkshop.Dispatch = function(_, command) return command == 99 end
UIWorkshop.On_CC_Mouse1 = function(self, bag, index)
	self.actions[#self.actions + 1] = { name = "On_CC_Mouse1", bag = bag, index = index }
end

local decorative_owner = dialog_class()
local no_op_owner = dialog_class()
no_op_owner.On_CC_Mouse1 = function() error("no-op Callback override must not receive input") end
local disabled_owner = dialog_class()
disabled_owner.On_CC_Mouse1 = function() error("disabled callback must not receive input") end
local highlight_owner = dialog_class()
highlight_owner.On_CC_Mouse1 = function() error("decorative cell must not receive input") end

local inventory_dialog = owner_instance(UIInventory, { actions = {}, sort_calls = {} })
local inventory_cells = container("actor_bag", inventory_dialog, true)
local hidden_panel = container("hidden_alias", inventory_dialog, false, false)
local empty_manual = container("empty_manual", inventory_dialog, true, true, false)
local equipped_manual = container("actor_equ", inventory_dialog, false, true, false)
local inventory_dialog_cells = {
	mode = "inventory",
	actions = inventory_dialog.actions,
	CC = { actor_bag = inventory_cells, hidden_alias = hidden_panel, empty_manual = empty_manual,
		actor_equ = equipped_manual },
}
setmetatable(inventory_dialog_cells, { __index = UIInventory })
for _, cell_container in pairs(inventory_dialog_cells.CC) do
	cell_container.owner = inventory_dialog_cells
end
-- This matches GAMMA's module-owned GUI instance used by the live adapter.
ui_inventory = { GUI = inventory_dialog_cells, UIInventory = UIInventory }

local new_game_dialog = owner_instance(UINewGame, { CC = {}, actions = {} })
local new_game_cells = container("inventory", new_game_dialog, true)
new_game_dialog.CC.inventory = new_game_cells
ui_mm_faction_select = { UINewGame = UINewGame }

local workshop_owner = owner_instance(UIWorkshop, { actions = {} })
local workshop_cells = container("inventory", workshop_owner, true)
local decorative_cells = container("tooltip", decorative_owner, true)
local no_op_cells = container("wheel", no_op_owner, true, true, true, { override_callback = true })
local disabled_cells = container("disabled", disabled_owner, true, true, true,
	{ disable_callback = { On_CC_Mouse1 = true } })
local highlight_cells = container("tooltip", highlight_owner, true, true, true, { disable_highlight = true })

utils_ui = { UICellItem = UICellItem, UICellContainer = UICellContainer }
local workshop_update = UICellContainer.Update

local module_environment = setmetatable({ _G = _G }, { __index = _G })
local module_chunk = assert(loadfile(script_path))
setfenv(module_chunk, module_environment)
module_chunk()
module_environment.install()

assert(inventory_cells.cell[1].cell.focusable == true, "existing inventory cell was not marked")
assert(inventory_cells.cell[1].cell.role == 1, "inventory cell role was not set")
assert(inventory_cells.prof.role == 2, "inventory container group role was not set")
assert(not inventory_cells.prof.focusable, "container group became directly focusable")
assert(not empty_manual.cell[1].cell.focusable, "empty manual cell became a dead focus stop")
assert(not equipped_manual.cell[1].cell.focusable, "empty equipment slot became a dead focus stop")

-- GAMMA fills pre-created equipment cells through AddItemManual, without
-- calling UICellItem.InitControls again. The compatibility hook must re-mark
-- the now-visible worn item, and remove it again when the slot is emptied.
assert(equipped_manual:AddItemManual(nil, "outfit", 1), "manual equipment item was not added")
assert(equipped_manual.cell[1].cell.focusable, "worn equipment item was not made focusable")
equipped_manual:RemoveItemManual(1)
assert(not equipped_manual.cell[1].cell.focusable, "removed equipment item remained focusable")
assert(inventory_dialog_cells.role == 3, "inventory dispatch role was not set")
assert(not inventory_dialog_cells:GetGamepadFocusable(), "inventory dialog became focusable")
assert(not inventory_dialog_cells._gamepad_compat_generic_dispatch,
	"known inventory dialog received the generic Dispatch wrapper")
new_game_dialog:Update()
assert(new_game_cells.cell[1].cell.focusable, "existing New Game cell was not marked")
assert(new_game_cells.prof.role == 2, "New Game container group role was not set")
assert(new_game_dialog.role == 3, "New Game dispatch role was not set")
assert(not new_game_dialog:GetGamepadFocusable(), "New Game dialog became focusable")
assert(not new_game_dialog._gamepad_compat_generic_dispatch,
	"known New Game dialog received the generic Dispatch wrapper")

local created = { container = workshop_cells, IsShown = function() return true end }
assert(workshop_cells.Callback == UICellContainer.Callback, "workshop container callback is not canonical")
assert(type(workshop_owner.On_CC_Mouse1) == "function", "workshop primary callback is missing")
assert(utils_ui.UICellItem.InitControls == utils_ui.UICellItem._gamepad_compat_init_wrapper,
	"cell InitControls wrapper was not installed")
utils_ui.UICellItem.InitControls(created)
assert(created.cell.focusable and created.cell.role == 1, "future actionable cell was not marked")

local decorative = item(true, true)
decorative.container = decorative_cells
utils_ui.UICellItem.InitControls(decorative)
assert(not decorative.cell.focusable, "ownerless/decorative cell became focusable")
local no_op = item(true, true)
no_op.container = no_op_cells
utils_ui.UICellItem.InitControls(no_op)
assert(not no_op.cell.focusable, "custom no-op callback became focusable")
local disabled = item(true, true)
disabled.container = disabled_cells
utils_ui.UICellItem.InitControls(disabled)
assert(not disabled.cell.focusable, "disabled primary callback became focusable")
local highlight = item(true, true)
highlight.container = highlight_cells
utils_ui.UICellItem.InitControls(highlight)
assert(not highlight.cell.focusable, "disable_highlight container became focusable")

-- The generic Update wrapper discovers already-created non-inventory owners.
utils_ui.UICellContainer.Update(workshop_cells)
assert(workshop_cells.prof.role == 2, "generic owner container group was not marked")
assert(workshop_owner.role == 3, "generic owner dispatch role was not set")
assert(not workshop_owner:GetGamepadFocusable(), "generic owner dialog became focusable")
utils_ui.UICellContainer.Update(decorative_cells)
utils_ui.UICellContainer.Update(no_op_cells)
utils_ui.UICellContainer.Update(disabled_cells)
utils_ui.UICellContainer.Update(highlight_cells)
assert(decorative_cells.prof.role ~= 2, "decorative container became a panel group")
assert(no_op_cells.prof.role ~= 2, "no-op container became a panel group")

assert(inventory_dialog_cells:Dispatch(0x4701, 0), "inventory accept was not handled")
assert(inventory_dialog.actions[1].name == "On_CC_Mouse1", "inventory accept used the wrong callback")
assert(inventory_dialog.actions[1].bag == "actor_bag" and inventory_dialog.actions[1].index == 1,
	"inventory dispatch did not match the exact focused container and cell")
assert(inventory_cells.hovered == 1, "focused cell hover was not refreshed")
assert(inventory_dialog_cells:Dispatch(0x4702, 0), "inventory context was not handled")
assert(inventory_dialog.actions[2].name == "On_CC_Mouse2", "inventory context used the wrong callback")
assert(inventory_dialog_cells:Dispatch(0x4703, 0), "inventory transfer was not handled")
assert(inventory_dialog.actions[3].name == "On_CC_Mouse1_DB", "inventory transfer used the wrong callback")

assert(workshop_owner:Dispatch(0x4701, 0), "generic accept was not handled")
assert(workshop_owner.actions[1].name == "On_CC_Mouse1", "generic accept used the wrong callback")
assert(workshop_owner:Dispatch(0x4703, 0), "generic transfer did not fall back to primary action")
assert(workshop_owner.actions[2].name == "On_CC_Mouse1", "generic transfer fallback used the wrong callback")
assert(workshop_owner:Dispatch(0x4702, 0) == false, "generic context must stay unsupported without a handler")
assert(decorative_owner.Dispatch == nil, "decorative owner received a dispatch adapter")
assert(no_op_owner.Dispatch == nil, "overridden no-op Callback received a dispatch adapter")

assert(inventory_dialog_cells:Dispatch(0x4705, 0), "sort-tab next was not handled")
assert(inventory_dialog_cells.sort_btn[2]:GetCheck(), "sort-tab next did not advance to the next tab")
assert(#inventory_dialog_cells.sort_calls == 2, "sort-tab dispatch skipped existing update calls")
assert(inventory_dialog_cells:Dispatch(0x4706, 0) == false, "panel cycling should use native group navigation")
assert(inventory_dialog_cells:Dispatch(99, 0), "unrelated dispatch did not preserve the prior handler")

local old_item_wrapper = utils_ui.UICellItem._gamepad_compat_init_wrapper
utils_ui.UICellItem.InitControls = function(self, ...)
	self.late_init_wrapper_called = true
	return old_item_wrapper(self, ...)
end
local old_update_wrapper = utils_ui.UICellContainer._gamepad_compat_update_wrapper
utils_ui.UICellContainer.Update = function(self, ...)
	self.late_update_wrapper_called = true
	return old_update_wrapper(self, ...)
end
module_environment.install()
local rewrapped_init = utils_ui.UICellItem._gamepad_compat_init_wrapper
local rewrapped_update = utils_ui.UICellContainer._gamepad_compat_update_wrapper
assert(rewrapped_init ~= old_item_wrapper and rewrapped_update ~= old_update_wrapper,
	"late optimization wrappers buried compatibility hooks")
module_environment.install()
assert(utils_ui.UICellItem._gamepad_compat_init_wrapper == rewrapped_init
	and utils_ui.UICellContainer._gamepad_compat_update_wrapper == rewrapped_update,
	"repeated install stacked duplicate wrappers")

inventory_cells.scroll_pos = 0
inventory_cells.scroll = { GetCurrentScrollPos = function() return 160 end }
inventory_cells.pad = { IsShown = function() return false end }
inventory_cells.cell[1].cell.focused = true
utils_ui.UICellContainer.Update(inventory_cells)
assert(inventory_cells.scroll_pos == 160, "gamepad focus did not sync native and GAMMA scroll positions")
inventory_cells.cell[1].cell.focused = false
inventory_cells.scroll_pos = 0
utils_ui.UICellContainer.Update(inventory_cells)
assert(inventory_cells.scroll_pos == 0, "unfocused mouse scroll path was changed")
inventory_cells.cell[1].cell.focused = true

assert(new_game_dialog:Dispatch(0x4701, 0), "New Game accept was not handled")
assert(new_game_dialog.actions[1].name == "On_CC_Mouse1", "New Game did not use its transfer callback")
assert(inventory_dialog_cells:Dispatch(0x4701, 0), "hidden aliases shadowed the focused cell")

inventory_cells.cell[1].cell.focused = false
workshop_cells.cell[1].cell.focused = false
assert(inventory_dialog_cells:Dispatch(0x4701, 0) == false, "unfocused cells received an action")
assert(workshop_owner:Dispatch(0x4701, 0) == false, "unfocused generic cell received an action")
inventory_cells.shown = false
hidden_panel.shown = false
empty_manual.cell[1].cell.focused = true
assert(inventory_dialog_cells:Dispatch(0x4701, 0) == false, "hidden panels or empty cells received an action")

assert(UICellContainer.Update ~= workshop_update, "container update wrapper was not installed")
print("gamepad_ui_compat: checks passed")
