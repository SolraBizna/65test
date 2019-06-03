#!/usr/bin/env lua5.3

local f = assert(io.open("gen/optable.csv","rb"))

local function parse_csv_line(line)
   local comma_positions = {}
   for pos in line:gmatch("(),") do
      comma_positions[#comma_positions+1] = pos
   end
   comma_positions[#comma_positions+1] = #line+1
   local ret = {line:sub(1,comma_positions[1]-1)}
   for n=1,#comma_positions-1 do
      ret[#ret+1] = line:sub(comma_positions[n]+1, comma_positions[n+1]-1)
   end
   return ret
end

-- Opcodes hardcoded here must be covered by non-generated tests
local covered = {
   [0x00]=true, -- BRK
   [0x20]=true, -- JSR xxxx
   [0x40]=true, -- RTI
   [0x60]=true, -- RTS
   [0x4C]=true, -- JMP xxxx
   [0x6C]=true, -- JMP (xxxx)
   [0x7C]=true, -- JMP (xxxx,X)
   -- one-byte NOPs
   [0x03]=true, [0x0B]=true, [0x13]=true, [0x1B]=true, [0x23]=true,
   [0x2B]=true, [0x33]=true, [0x3B]=true, [0x43]=true, [0x4B]=true,
   [0x53]=true, [0x5B]=true, [0x63]=true, [0x6B]=true, [0x73]=true,
   [0x7B]=true, [0x83]=true, [0x8B]=true, [0x93]=true, [0x9B]=true,
   [0xA3]=true, [0xAB]=true, [0xB3]=true, [0xBB]=true, [0xC3]=true,
   [0xD3]=true, [0xE3]=true, [0xEA]=true, [0xEB]=true, [0xF3]=true,
   [0xFB]=true,
   -- flag setting instructions
   [0x18]=true, [0x38]=true, [0x58]=true, [0x78]=true, [0xB8]=true,
   [0xD8]=true, [0xF8]=true,
   -- A shifts
   [0x0A]=true, [0x2A]=true, [0x4A]=true, [0x6A]=true,
   -- stack ops
   [0x08]=true, [0x28]=true, [0x48]=true, [0x5A]=true, [0x68]=true,
   [0x7A]=true, [0xDA]=true, [0xFA]=true,
   -- transfers
   [0xAA]=true, [0xBA]=true, [0x8A]=true, [0x9A]=true, [0xA8]=true,
   [0x98]=true,
   -- implied decrement
   [0x3A]=true, [0xCA]=true, [0x88]=true,
   -- implied increment
   [0x1A]=true, [0xE8]=true, [0xC8]=true,
   -- RMBx
   [0x07]=true, [0x17]=true, [0x27]=true, [0x37]=true, [0x47]=true,
   [0x57]=true, [0x67]=true, [0x77]=true,
   -- SMBx
   [0x87]=true, [0x97]=true, [0xA7]=true, [0xB7]=true, [0xC7]=true,
   [0xD7]=true, [0xE7]=true, [0xF7]=true,
   -- BBRx
   [0x0F]=true, [0x1F]=true, [0x2F]=true, [0x3F]=true, [0x4F]=true,
   [0x5F]=true, [0x6F]=true, [0x7F]=true,
   -- BBSx
   [0x8F]=true, [0x9F]=true, [0xAF]=true, [0xBF]=true, [0xCF]=true,
   [0xDF]=true, [0xEF]=true, [0xFF]=true,
   -- branches
   [0x10]=true, [0x30]=true, [0x50]=true, [0x70]=true, [0x80]=true,
   [0x90]=true, [0xB0]=true, [0xD0]=true, [0xF0]=true,
}

-- leave out the implied modes
local MODES = {"abs","absx","absy","immediate","zp","zpi","zpi","zpiy","zpx","zpxi","zpy"}
local TEMPLATES = {}
for i,mode in ipairs(MODES) do
   local f = assert(io.open("util/am_tmpls/"..mode..".65c", "rb"))
   local a = assert(f:read("*a"))
   f:close()
   TEMPLATES[mode] = a
end

local headerline = parse_csv_line(f:read("*l"))
for l in f:lines() do
   local l = parse_csv_line(l)
   assert(l[1]:match("^[A-Z][A-Z][A-Z]$"))
   local mnemonic = l[1]
   for n=2,#l do
      if l[n] == "  " then
         -- do nothing
      elseif l[n]:match("^[0-9A-F/]+$") then
         local mode = headerline[n]
         if TEMPLATES[mode] then
            local i = 1
            for opcode in l[n]:gmatch("[0-9A-F][0-9A-F]") do
               local opcode_n = tonumber(opcode, 16)
               assert(not covered[opcode_n])
               covered[opcode_n] = true
               local testno
               if i == 1 then testno = ""
               else testno = "_"..i end
               local f = assert(io.open("tests/gen/"..mnemonic:lower().."_"..mode..testno..".65c","wb"))
               assert(f:write((TEMPLATES[mode]:gsub("XXX",opcode))))
               f:close()
               i = i + 1
            end
         else
            for opcode in l[n]:gmatch("[0-9A-F][0-9A-F]") do
               local opcode_n = tonumber(opcode, 16)
               if not covered[opcode_n] then
                  print("(skipping "..mnemonic.." "..mode.." $"..opcode..")")
               end
            end
         end
      else
         error(mnemonic.." has a weird opcode")
      end
   end
end
f:close()

for _,status in ipairs{"CLI","SEI"} do
   local f = assert(io.open("tests/wai_"..status:lower()..".65c","rb"))
   local a = assert(f:read("*a"))
   f:close()
   for phase=1,90 do
      local f = assert(io.open("tests/gen/irqphase/"..status:lower().."_wai_"..phase..".65c","wb"))
      assert(f:write(a))
      f:close()
      local f = assert(io.open("tests/gen/irqphase/"..status:lower().."_wai_"..phase..".job.tmpl","wb"))
      assert(f:write(('{"show_cycles":true,"max_cycles":200,"init":[$$$$],"terminate_on_bad_write":false,"irq":[%i,%i]}'):format(40+phase, 80+phase//15)))
      f:close()
   end
end

-- the data we write actually doesn't change, but this does trick make into
-- reloading the file and re-checking the dependencies
local f = assert(io.open("generated_tests.mk", "wb"))
f:write[[
test: $(patsubst tests/%.65c,tmp/%.test,$(shell find tests -name \*.65c))
	util/check_test_results.sh
]]
f:close()

local missing = 0
for n=0,255 do
   if not covered[n] then
      missing = missing + 1
   end
end

if missing > 0 then
   print("Not all opcodes are covered yet.")
   print("   0123456789ABCDEF");
   for n=0,255 do
      if n % 16 == 0 then io.write(("%01X_ "):format(n>>4)) end
      if not covered[n] then
         io.write("x")
      else
         io.write(".")
      end
      if n % 16 == 15 then io.write("\n") end
   end
else
   print("Looks like all opcodes are covered.")
end
