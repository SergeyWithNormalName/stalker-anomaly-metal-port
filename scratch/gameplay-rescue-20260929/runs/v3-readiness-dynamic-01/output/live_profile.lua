local output = assert(os.getenv('PERF_GAP_PHASES'))
local warmup_ms = tonumber(os.getenv('RESCUE_WARMUP_MS')) or 30000
local measure_ms = tonumber(os.getenv('RESCUE_MEASURE_MS')) or 60000
local save_control = os.getenv('RESCUE_SAVE_CONTROL') == '1'
local trace = assert(io.open(output .. '.device.csv', 'w'))
trace:write('engine_frame,wall_ms,epoch_s,phase,camera_x,camera_y,camera_z,dir_x,dir_y,dir_z,device_time_factor,game_time_factor,width,height,level,weather,calendar\n')

local started = false
local phase_index = 0
local phase_start = 0
local environment_trace = assert(io.open(output .. '.weather.csv', 'w'))
environment_trace:write('marker,paused,sky_texture,clouds_texture,weight,far_plane,fog_distance,fog_density,rain_density,wind_velocity,ambient,hemisphere,sun_color,sun_dir,fog_color\n')
local function vector_string(key)
    local value = weather.get_value_vector(key)
    return string.format('%.9g/%.9g/%.9g', value.x, value.y, value.z)
end
local function state_string()
    local clock = game.get_game_time()
    local y, mo, day, h, mi, se = clock:get()
    return string.format('%d-%02d-%02dT%02d:%02d:%02d', y, mo, day, h, mi, se)
end

local function marker(name)
    local d = device()
    local p = d.cam_pos
    local v = d.cam_dir
    local f = assert(io.open(output, 'a'))
    f:write(string.format('%s,frame=%d,wall_ms=%d,level=%s,weather=%s,calendar=%s,device_time_factor=%s,game_time_factor=%.9g,drawable=%dx%d,camera=%.6f/%.6f/%.6f,dir=%.6f/%.6f/%.6f\n',
        name, d.frame, d:time_continual(), level.name(), level.get_weather(), state_string(), get_console():get_string('time_factor'), level.get_time_factor(), d.width, d.height, p.x, p.y, p.z, v.x, v.y, v.z))
    f:close()
    environment_trace:write(string.format('%s,%s,%s,%s,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%s,%s,%s,%s,%s\n',
        name, tostring(weather.is_paused()), weather.get_value_string('sky_texture'), weather.get_value_string('clouds_texture'),
        level.get_weather_weight(), weather.get_value_numric('far_plane'), weather.get_value_numric('fog_distance'),
        weather.get_value_numric('fog_density'), weather.get_value_numric('rain_density'), weather.get_value_numric('wind_velocity'),
        vector_string('ambient_color'), vector_string('hemisphere_color'), vector_string('sun_color'), vector_string('sun_dir'), vector_string('fog_color')))
    environment_trace:flush()
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

    marker('loaded_state')
    -- Pin the render view and environment. Device time remains at 1, so AI and
    -- NPC simulation continue while calendar/weather are held at a fixed state.
    local fixed_position = vector():set(-221.082565, -18.284866, -168.182037)
    local fixed_hpb = vector():set(0.0, 0.0, 0.0) -- forward +Z
    get_console():execute('rs_stats off')
    level.set_cam_custom_position_direction(fixed_position, fixed_hpb, 1, true, false)
    weather.pause(false)
    level.set_weather('w_clear2', true)
    level.set_weather_weight(0.0)
    weather.sun_time(8, 3)
    weather.pause(true)
    level.set_time_factor(6.0)

    phase_start = device():time_continual()
    marker('warmup_start')
    level.add_call(function()
        local d = device()
        local p, v = d.cam_pos, d.cam_dir
        trace:write(string.format('%d,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%s,%.9g,%d,%d,%s,%s,%s\n',
            d.frame, d:time_continual(), os.time(), phase_index, p.x, p.y, p.z, v.x, v.y, v.z, get_console():get_string('time_factor'), level.get_time_factor(), d.width, d.height, level.name(), level.get_weather(), state_string()))

        if phase_index == 0 and d:time_continual() - phase_start >= warmup_ms and d.precache_frame == 0 and (not level.is_loading_screen_active or not level.is_loading_screen_active()) then
            phase_index = 1
            phase_start = d:time_continual()
            marker('measure_start')
            if save_control then get_console():execute('save rescue-cordon-control') end
        elseif phase_index == 1 and d:time_continual() - phase_start >= measure_ms then
            phase_index = 2
            marker('measure_end')
            trace:flush()
            get_console():execute('rs_stats on')
            phase_start = d:time_continual()
        elseif phase_index == 2 and d:time_continual() - phase_start >= 2000 then
            phase_index = 3
            marker('screenshot')
            get_console():execute('screenshot')
            phase_start = d:time_continual()
        elseif phase_index == 3 and d:time_continual() - phase_start >= 15000 then
            marker('finished')
            trace:flush()
            trace:close()
            environment_trace:close()
            get_console():execute('quit')
            return true
        end
        return false
    end, function() end)
end

function parity_harness_register()
    RegisterScriptCallback('actor_on_first_update', first_update)
end
