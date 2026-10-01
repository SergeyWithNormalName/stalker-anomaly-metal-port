local output = assert(os.getenv('PERF_GAP_PHASES'))
local warmup_ms = 60000
local switched_r1_off = false
local measure_ms = 20000
local visual_tail_ms = tonumber(os.getenv('XRAY_PARITY_VISUAL_TAIL_MS') or '5000')
assert(visual_tail_ms and visual_tail_ms >= 5000 and visual_tail_ms <= 90000)
local trace = assert(io.open(output .. '.device.csv', 'w'))
trace:write('engine_frame,wall_ms,epoch_s,phase,camera_x,camera_y,camera_z,dir_x,dir_y,dir_z,device_time_factor,game_time_factor,width,height,level,weather,calendar\n')
local environment = assert(io.open(output .. '.weather.csv', 'w'))
environment:write('marker,epoch_s,paused,sky_texture,clouds_texture,weight,far_plane,fog_distance,fog_density,rain_density,wind_velocity,ambient,hemisphere,sun_color,sun_dir,fog_color\n')
local started = false
local phase = 0
local phase_start = 0
local function state_time()
    local clock = game.get_game_time()
    local y, mo, d, h, mi, s = clock:get()
    return string.format('%04d-%02d-%02dT%02d:%02d:%02d', y, mo, d, h, mi, s)
end
local function vstr(key)
    local v = weather.get_value_vector(key)
    return string.format('%.9g/%.9g/%.9g', v.x, v.y, v.z)
end
local function marker(name)
    local d = device(); local p = d.cam_pos; local v = d.cam_dir
    local f = assert(io.open(output, 'a'))
    f:write(string.format('%s,frame=%d,wall_ms=%d,epoch_s=%d,level=%s,weather=%s,calendar=%s,device_time_factor=%s,game_time_factor=%.9g,drawable=%dx%d,camera=%.6f/%.6f/%.6f,dir=%.6f/%.6f/%.6f\n',
        name, d.frame, d:time_continual(), os.time(), level.name(), level.get_weather(), state_time(),
        get_console():get_string('time_factor'), level.get_time_factor(), d.width, d.height,
        p.x, p.y, p.z, v.x, v.y, v.z))
    f:close()
    environment:write(string.format('%s,%d,%s,%s,%s,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%s,%s,%s,%s,%s\n',
        name, os.time(), tostring(weather.is_paused()), weather.get_value_string('sky_texture'),
        weather.get_value_string('clouds_texture'), level.get_weather_weight(),
        weather.get_value_numric('far_plane'), weather.get_value_numric('fog_distance'),
        weather.get_value_numric('fog_density'), weather.get_value_numric('rain_density'),
        weather.get_value_numric('wind_velocity'), vstr('ambient_color'), vstr('hemisphere_color'),
        vstr('sun_color'), vstr('sun_dir'), vstr('fog_color')))
    environment:flush()
end
local function first_update()
    if started or level.name() == 'fake_start' then return end
    started = true
    local factor = tonumber(get_console():get_string('time_factor'))
    if not factor or math.abs(factor - 1.0) > 0.000001 then
        marker('FAILED_device_clock_not_one'); trace:close(); get_console():execute('quit'); return
    end
    marker('loaded_state')
    get_console():execute('r2_allow_r1_lights on')
    marker('qualification_R1_on')
    local fixed = vector():set(-221.082565, -18.284866, -168.182037)
    local hpb = vector():set(0.0, 0.0, 0.0)
    get_console():execute('rs_stats off')
    level.set_cam_custom_position_direction(fixed, hpb, 1, true, false)
    weather.pause(false)
    level.set_weather('w_clear2', true)
    level.set_weather_weight(0.0)
    weather.sun_time(8, 3)
    weather.pause(true)
    level.set_time_factor(6.0)
    phase_start = device():time_continual()
    marker('warmup_start')
    level.add_call(function()
        local d = device(); local p = d.cam_pos; local v = d.cam_dir
        trace:write(string.format('%d,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%s,%.9g,%d,%d,%s,%s,%s\n',
            d.frame, d:time_continual(), os.time(), phase, p.x, p.y, p.z, v.x, v.y, v.z,
            get_console():get_string('time_factor'), level.get_time_factor(), d.width, d.height,
            level.name(), level.get_weather(), state_time()))
        local elapsed = d:time_continual() - phase_start
        if phase == 0 and elapsed >= warmup_ms and d.precache_frame == 0 and
           (not level.is_loading_screen_active or not level.is_loading_screen_active()) then
            phase = 1; phase_start = d:time_continual(); marker('measure_start')
        elseif phase == 1 and elapsed >= measure_ms then
            phase = 2; marker('measure_end'); trace:flush(); phase_start = d:time_continual()
        elseif phase == 2 and elapsed >= 30000 and not switched_r1_off then
            get_console():execute('r2_allow_r1_lights off')
            switched_r1_off = true
            marker('qualification_R1_off')
        elseif phase == 2 and elapsed >= visual_tail_ms then
            phase = 3; marker('post_window_end'); trace:flush(); trace:close(); environment:close()
            get_console():execute('quit'); return true
        end
        return false
    end, function() end)
end
function parity_harness_register()
    RegisterScriptCallback('actor_on_first_update', first_update)
end
