local output = assert(os.getenv('RESCUE_MOTION_OUT'))
local base_x, base_y, base_z = -221.082565, -18.284866, -168.182037
local started = false
local t0 = 0
local last_stage = -1

local function state_string()
    local clock = game.get_game_time()
    local y, mo, day, h, mi, se = clock:get()
    return string.format('%04d-%02d-%02dT%02d:%02d:%02d', y, mo, day, h, mi, se)
end

local function marker(name)
    local d = device()
    local p, v = d.cam_pos, d.cam_dir
    local f = assert(io.open(output .. '/motion.csv', 'a'))
    f:write(string.format('%s,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%s,%s\n',
        name, d.frame, d:time_continual(), p.x, p.y, p.z, v.x, v.y, v.z,
        level.get_weather(), state_string()))
    f:close()
end

local function first_update()
    if started or level.name() == 'fake_start' then return end
    started = true
    local factor = tonumber(get_console():get_string('time_factor'))
    if not factor or math.abs(factor - 1.0) > 0.000001 then
        marker('FAILED_device_clock')
        get_console():execute('quit')
        return
    end
    get_console():execute('rs_stats off')
    weather.pause(false)
    level.set_weather('w_clear2', true)
    level.set_weather_weight(0.0)
    weather.sun_time(8, 3)
    weather.pause(true)
    level.set_time_factor(6.0)
    t0 = device():time_continual()
    marker('motion_start')

    level.add_call(function()
        local elapsed = (device():time_continual() - t0) / 1000.0
        -- A small camera path stays in the known corridor: rotate, move 0.75 m
        -- forward, rotate the other way, return, then restore the oracle view.
        local z = base_z
        local heading = 0.0
        local stage
        if elapsed < 4 then
            stage = 0
        elseif elapsed < 9 then
            stage, heading = 1, (elapsed - 4) * 0.10
        elseif elapsed < 14 then
            stage, heading = 2, 0.50
            z = base_z + 0.75 * math.min(1.0, (elapsed - 9) / 5.0)
        elseif elapsed < 19 then
            stage, heading = 3, 0.50 - (elapsed - 14) * 0.20
            z = base_z + 0.75
        elseif elapsed < 24 then
            stage, heading = 4, -0.50
            z = base_z + 0.75 * (1.0 - math.min(1.0, (elapsed - 19) / 5.0))
        else
            stage, heading = 5, 0.0
        end
        level.set_cam_custom_position_direction(
            vector():set(base_x, base_y, z), vector():set(heading, 0.0, 0.0), 1, true, false)
        if stage ~= last_stage then
            last_stage = stage
            marker('stage_' .. tostring(stage))
        end
        if elapsed >= 27 then
            marker('motion_end')
            get_console():execute('quit')
            return true
        end
        return false
    end, function() end)
end

function parity_harness_register()
    RegisterScriptCallback('actor_on_first_update', first_update)
end
