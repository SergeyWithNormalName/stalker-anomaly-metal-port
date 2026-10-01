-- Draft only: save a Windows-created fresh-game control after the world settles.
-- Wire through XRAY_PARITY_HARNESS_SCRIPT in an isolated cloned runtime.
-- This profile records control markers; it does not claim an FPS interval.
local output = assert(os.getenv('PERF_GAP_PHASES'))
local warmup_ms = 20000
local save_tail_ms = 5000
local phase = 0
local started = false
local start_ms = 0
local trace = assert(io.open(output .. '.control.csv', 'w'))
trace:write('marker,epoch_s,frame,wall_ms,level,weather,game_time_factor,device_time_factor,width,height,camera_x,camera_y,camera_z,dir_x,dir_y,dir_z,calendar\n')
local function calendar()
    local clock = game.get_game_time()
    local y, mo, d, h, mi, s = clock:get()
    return string.format('%04d-%02d-%02dT%02d:%02d:%02d', y, mo, d, h, mi, s)
end
local function marker(name)
    local d = device(); local p = d.cam_pos; local v = d.cam_dir
    trace:write(string.format('%s,%d,%d,%d,%s,%s,%.9g,%s,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%s\n',
        name, os.time(), d.frame, d:time_continual(), level.name(), level.get_weather(),
        level.get_time_factor(), get_console():get_string('time_factor'), d.width, d.height,
        p.x, p.y, p.z, v.x, v.y, v.z, calendar()))
    trace:flush()
end
local function first_update()
    if started or level.name() == 'fake_start' then return end
    started = true
    local device_factor = tonumber(get_console():get_string('time_factor'))
    if not device_factor or math.abs(device_factor - 1.0) > 0.000001 then
        marker('FAILED_device_clock_not_one')
        trace:close()
        get_console():execute('quit')
        return
    end
    -- Same fixed view/environment knobs as the matched profile; ALife remains active.
    get_console():execute('rs_stats off')
    level.set_cam_custom_position_direction(
        vector():set(-221.082565, -18.284866, -168.182037),
        vector():set(0.0, 0.0, 0.0), 1, true, false)
    weather.pause(false)
    level.set_weather('w_clear2', true)
    level.set_weather_weight(0.0)
    weather.sun_time(8, 3)
    weather.pause(true)
    level.set_time_factor(6.0)
    start_ms = device():time_continual()
    marker('actor_loaded_warmup_start')
    level.add_call(function()
        local d = device()
        local elapsed = d:time_continual() - start_ms
        if phase == 0 and elapsed >= warmup_ms and d.precache_frame == 0 and
           (not level.is_loading_screen_active or not level.is_loading_screen_active()) then
            phase = 1
            start_ms = d:time_continual()
            marker('settled_save_request')
            -- Server handles M_SAVE_GAME asynchronously; wait for its log receipt after this call.
            get_console():execute('save parity-windows-fresh-cordon')
        elseif phase == 1 and elapsed >= save_tail_ms then
            phase = 2
            marker('save_tail_end')
            trace:close()
            get_console():execute('quit')
            return true
        end
        return false
    end, function() end)
end
function parity_harness_register()
    RegisterScriptCallback('actor_on_first_update', first_update)
end
