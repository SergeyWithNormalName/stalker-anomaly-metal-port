local log_path = assert(os.getenv('XRAY_WHOLE_LIGHT_VISIBILITY_HARNESS_LOG'))
local log = assert(io.open(log_path, 'w'))
local first_sample_frame = tonumber(os.getenv('XRAY_METAL_WHOLE_LIGHT_VISIBILITY_START_FRAME')) or 300
local last_sample_frame = first_sample_frame + 11
local drain_ms = 1500
local window_started = false
local window_ended = false
local end_time_ms = nil
local started = false
local first_update_ms = nil
local first_update_frame = nil

local function state_string()
    local clock = game.get_game_time()
    local y, mo, day, h, mi, se = clock:get()
    return string.format('%d-%02d-%02dT%02d:%02d:%02d', y, mo, day, h, mi, se)
end

local function marker(name, extra)
    local d = device()
    local p = d.cam_pos
    local v = d.cam_dir
    log:write(string.format('%s,frame=%d,wall_ms=%d,level=%s,weather=%s,calendar=%s,device_time_factor=%s,game_time_factor=%.9g,drawable=%dx%d,camera=%.6f/%.6f/%.6f,dir=%.6f/%.6f/%.6f%s\n',
        name, d.frame, d:time_continual(), level.name(), level.get_weather(), state_string(),
        get_console():get_string('time_factor'), level.get_time_factor(), d.width, d.height,
        p.x, p.y, p.z, v.x, v.y, v.z, extra and (',' .. extra) or ''))
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

    marker('observer_loaded_state')
    local fixed_position = vector():set(-221.082565, -18.284866, -168.182037)
    local fixed_hpb = vector():set(0.0, 0.0, 0.0) -- recorded +Z observation direction
    get_console():execute('rs_stats off')
    level.set_cam_custom_position_direction(fixed_position, fixed_hpb, 1, true, false)
    weather.pause(false)
    level.set_weather('w_clear2', true)
    level.set_weather_weight(0.0)
    weather.sun_time(8, 3)
    weather.pause(true)
    level.set_time_factor(6.0)

    first_update_ms = device():time_continual()
    first_update_frame = device().frame
    level.add_call(function()
        local d = device()
        local elapsed = d:time_continual() - first_update_ms
        if not window_started and d.frame >= first_sample_frame then
            window_started = true
            marker('observer_window_start', string.format('first_sample_frame=%d,last_sample_frame=%d,first_update_frame=%d',
                first_sample_frame, last_sample_frame, first_update_frame))
        end
        if not window_ended and d.frame > last_sample_frame then
            window_ended = true
            end_time_ms = d:time_continual()
            marker('observer_window_end', string.format('last_sample_frame=%d,observed_end_frame=%d',
                last_sample_frame, d.frame))
        end
        if window_ended and d:time_continual() - end_time_ms >= drain_ms then
            marker('observer_drain_complete')
            log:close()
            get_console():execute('quit')
            return true
        end
        if elapsed >= 120000 then
            marker(window_started and 'FAILED_window_drain_timeout' or 'FAILED_window_not_reached',
                string.format('first_sample_frame=%d,last_sample_frame=%d,observed_frame=%d',
                    first_sample_frame, last_sample_frame, d.frame))
            log:close()
            get_console():execute('quit')
            return true
        end
        return false
    end, function() end)
end

function parity_harness_register()
    RegisterScriptCallback('actor_on_first_update', first_update)
end
