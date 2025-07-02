--[[
this file tweens inputs
it needs to run with a default handler.

v0.2, 2025-07-01
]]


local flux = require "flux/flux"

local DEBUG = true
local DELTA_TIME = 100  -- Default delta time for updates in milliseconds
local DEFAULT_EASING = "linear"  -- Default easing is "linear"
local DEFAULT_DURATION = "1000"  -- Default duration is 1000 milliseconds

local channelStates = {}  -- table to hold channel states

local function isValidEasing(name)
    return type(name) == "string" and (name == "thru" or type(flux.easing[name]) == "function")
end

function log(...)
    if DEBUG then print(...) end
end

local function parseChannelSpec(str)
    local result = {}

    if not str then
        return nil, "Input channel is nil"
    end

    -- Match channel number (must always be present)
    result.chan = str:match("^(%d+)")
    if not result.chan then
        return nil, "Missing channel number"
    end

    result.easing = str:match("^%d+%.([%a_]+)")
    -- Validate easing
    if result.easing and not isValidEasing(result.easing) then
        return nil, "Invalid easing: " .. tostring(result.easing)
    end

    -- Extract duration if present (as the last part)
    result.duration = str:match("^%d+%.?[%a_]*%.(%d+)$")

    -- Coerce duration to numeric for comparison
    local numericDuration = tonumber(result.duration)

    -- Normalize "immediate" behavior
    if result.easing == "thru" then
        result.duration = "0"
    elseif numericDuration == 0 then
        result.easing = "thru"
    end

    -- Assign defaults if missing
    result.easing = result.easing or DEFAULT_EASING
    result.duration = result.duration or DEFAULT_DURATION

    return result
end

local function updateTween()
    flux.update(DELTA_TIME / 1000)
end

function tween(value)
   
    print("TWEEN: _" .. input_channel() .. "_ @ " .. value)

    -- get the input channel and validate it
    local str = input_channel()
    
    local parsed, err = parseChannelSpec(str)
    if not parsed then
        log("Error parsing channel spec: " .. err)
        return
    end

    log("Parsed channel: " .. parsed.chan .. ", easing: " .. parsed.easing .. ", duration: " .. parsed.duration)

    if channelStates[parsed.chan] then
        local state = channelStates[parsed.chan]
        if state.tween then
            -- Stop the existing tween if it exists
            state.tween:stop()
            log("Stopped existing tween for channel " .. parsed.chan)
        end
    else
        -- Initialize the channel state if it doesn't exist
        channelStates[parsed.chan] = { chan = parsed.chan, value = 0, tween = nil }
    end

    -- set the value to tween to.
    local dst = {
        value = value
    }

    if (parsed.easing == "thru") or (dst.value == channelStates[parsed.chan].value) then
        -- If easing is "thru", or the destination value is the same as the current value, we treat it as an immediate action
        channelStates[parsed.chan].value = dst.value
        output("out" .. parsed.chan, dst.value)
        log("Immediate action for channel " .. parsed.chan .. ": " .. dst.value)
        return
    end

    -- Create a new tween object
    local tween_obj = flux.to(channelStates[parsed.chan], tonumber(parsed.duration) / 1000, dst)
        :ease(parsed.easing)
        :onupdate(function()
            output("out" .. parsed.chan, channelStates[parsed.chan].value)
            log("Tween update for channel " .. parsed.chan .. ": " .. channelStates[parsed.chan].value)
        end)
        :oncomplete(function()
            channelStates[parsed.chan].tween = nil
            log("Tween complete for channel " .. parsed.chan)
        end)
    
    -- Store the tween in the channel state
    channelStates[parsed.chan].tween = tween_obj
   
end


interval(updateTween, DELTA_TIME)