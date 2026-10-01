local output = assert(os.getenv('XRAY_ATTRIBUTION_MARKERS'))
local log = assert(io.open(output, 'w'))
local started = false

local function state_string()
    local clock = game.get_game_time()
    local y, mo, day, h, mi, se = clock:get()
    return string.format('%d-%02d-%02dT%02d:%02d:%02d', y, mo, day, h, mi, se)
end

local function marker(name)
    local d = device()
    local p = d.cam_pos
    local v = d.cam_dir
    log:write(string.format('%s,frame=%d,wall_ms=%d,level=%s,weather=%s,calendar=%s,device_time_factor=%s,game_time_factor=%.9g,drawable=%dx%d,camera=%.6f/%.6f/%.6f,dir=%.6f/%.6f/%.6f\n',
        name, d.frame, d:time_continual(), level.name(), level.get_weather(), state_string(), get_console():get_string('time_factor'), level.get_time_factor(), d.width, d.height, p.x, p.y, p.z, v.x, v.y, v.z))
    log:flush()
end

local function first_update()
    if started or level.name() == 'fake_start' then return end
    started = true
    local device_factor = tonumber(get_console():get_string('time_factor'))
    if not device_factor or math.abs(device_factor - 1.0) > 0.000001 then
        marker('FAILED_device_clock_not_one')
        log:close()
        get_console():execute('quit')
        return
    end

    get_console():execute('rs_stats off')
    local fixed_position = vector():set(-221.082565, -18.284866, -168.182037)
    local fixed_hpb = vector():set(0.0, 0.0, 0.0)
    level.set_cam_custom_position_direction(fixed_position, fixed_hpb, 1, true, false)
    weather.pause(false)
    level.set_weather('w_clear2', true)
    level.set_weather_weight(0.0)
    weather.sun_time(8, 3)
    weather.pause(true)
    level.set_time_factor(6.0)

    marker('loaded_state')
    local warm_start = device():time_continual()
    marker('attribution_warmup_start')
    level.add_call(function()
        local d = device()
        if d:time_continual() - warm_start >= 20000 then
            marker('attribution_window_start')
            local measurement_start = d:time_continual()
            level.add_call(function()
                local current = device()
                if current:time_continual() - measurement_start >= 10000 then
                    marker('attribution_window_end')
                    log:close()
                    get_console():execute('quit')
                    return true
                end
                return false
            end, function() end)
            return true
        end
        return false
    end, function() end)
end

function parity_harness_register()
    RegisterScriptCallback('actor_on_first_update', first_update)
end
