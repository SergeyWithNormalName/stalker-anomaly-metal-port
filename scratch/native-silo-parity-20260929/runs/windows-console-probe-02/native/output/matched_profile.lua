-- Configuration diagnostic only; no FPS phase or gameplay acceptance.
function parity_harness_register()
    local output = assert(os.getenv('PERF_GAP_PHASES'))
    local f = assert(io.open(output .. '.console.csv', 'w'))
    local c = get_console()
    local controls = {
        {'renderer', nil}, {'r2_allow_r1_lights', 'off'},
        {'r2_ssa_lod_a', '64'}, {'r2_ssa_lod_b', '48'},
        {'r__ssa_discard', '7'}, {'r__ssa_discard_exp', '0.5'}, {'r__ssa_discard_fade_k', '4'},
        {'r2_lights_details', 'off'}, {'r2_mt', 'on'},
        {'r2_shadow_lod_min', '0.02'}, {'r2_shadow_omnipart_vischeck', '1'},
        {'r__hom_dynamic', 'off'}, {'r2_smap_size', nil}, {'r2_sun', nil},
        {'r3_msaa', nil}, {'r2_ssao_mode', nil}, {'r2_ssao', nil},
        {'r__detail_density', nil}, {'r__detail_radius', nil}, {'fov', nil}, {'r__geometry_lod', nil}
    }
    f:write('name,registered_before,before,set_attempted,registered_after,after\n')
    f:flush()
    for _, row in ipairs(controls) do
        local ok, before = pcall(function() return c:get_string(row[1]) end)
        if not ok then before = nil end
        if row[2] then c:execute(row[1] .. ' ' .. row[2]) end
        local ok_after, after = pcall(function() return c:get_string(row[1]) end)
        if not ok_after then after = nil end
        f:write(string.format('%s,%s,%s,%s,%s,%s\n', row[1],tostring(before ~= nil),tostring(before),tostring(row[2] ~= nil),tostring(after ~= nil),tostring(after)))
        f:flush()
    end
    c:execute('cfg_save')
    f:write('probe_complete,true\n');f:close()
    local done=assert(io.open(output,'w'));done:write('console_probe_complete,no_fps_claim\n');done:close()
    c:execute('quit')
end
