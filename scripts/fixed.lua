
local DEBUG = true

function log(...)
    if DEBUG then print(...) end
end

function fixed(value)

    local str = input_channel()

    log("FIXED: _" .. input_channel() .. "_ @ " .. value)

    if not str then
        log("no input_channel")
        return 
    end

    local chan, suffix = str:match("^(.-)%.(%d+)$")
    local fixed_value = tonumber(suffix)

    log("fixed_value: "..fixed_value)

    if fixed_value and fixed_value >= 0 and fixed_value <= 100 then
        -- valid number in range
        log("Valid fixed output:"..fixed_value.." on chan:"..chan)
        output("out" .. chan, fixed_value/100.0)
    else
        -- invalid number or out of range
        log("Invalid output")
        output("out" .. chan, 0)
    end
end