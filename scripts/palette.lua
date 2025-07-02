--[[
takes a number that relates to the Rosco number and returns the corresponding RGB values.
]]

local gels = require("scripts/gels")

local gelLookup = {}

local function build_lookup_by_number(array)
  local lookup = {}
  for _, entry in ipairs(array) do
    if entry.gel_number then
      lookup[entry.gel_number] = entry
    end
  end
  return lookup
end

local function get_rgb_value(rgb)
  return rgb / 255.0
end

function palette_convert(value)
  
    local input = input_channel()

    print("Palette Convert: " .. input .. " @ " .. value)
  
    -- Match channel number (must always be present)
    channelStr = input:match("^ch(%d+)")
   
    if not channelStr then
        print("Invalid input: expecting channel format 'ch<number>' like 'ch0'")
        return
    end

    local gelStr = input:match("%.?(R%d%d)$")
    if not gelStr then
        gelStr = "R" .. string.format("%02d", tonumber(value) * 127)
    end

    -- Now both variables are set
    print("Channel:", channelStr)
    print("Gel Number:", gelStr)

    local gel = gelLookup[gelStr]

    if not gel then
        print("Gel not found for number:", gelStr)
        return
    end

    print ("Gel:", gel.name)
    local channelNum = tonumber(channelStr)
    
    output("out" .. channelNum, get_rgb_value(gel.rgb[1]))
    output("out" .. (channelNum + 1), get_rgb_value(gel.rgb[2]))
    output("out" .. (channelNum + 2), get_rgb_value(gel.rgb[3]))
end

gelLookup = build_lookup_by_number(gels)