#!/usr/bin/env lua5.3

local base64 = require "base64"

if #arg ~= 3 then
   print("usage: makejob input.tmpl input.bin output.json")
   os.exit(1)
end

local tmplpath = arg[1]
local binpath = arg[2]
local outpath = arg[3]

local f = assert(io.open(binpath,"rb"))
local bin = assert(f:read(65536))
f:close()
assert(#bin == 65536)

-- try to make a short set of init records that puts this test image into place
local function is_significant(addr)
   if addr > 0xFFFF then return false end
   local c = bin:sub(addr+1,addr+1)
   if addr == 0xFFFD then return c ~= "\x02"
   else return c ~= "\x00"
   end
end
local init_records = {}
local base_addr = 0
while base_addr < 65536 do
   repeat
      if is_significant(base_addr) then break
      else base_addr = base_addr + 1
      end
   until base_addr >= 65536
   if base_addr >= 65536 then break end
   -- next init record starts here
   local end_addr = base_addr + 1
   local insignificant_run = 0
   repeat
      if is_significant(end_addr) then
         insignificant_run = 0
      else
         insignificant_run = insignificant_run + 1
         if insignificant_run > 16 then
            end_addr = end_addr - insignificant_run
            goto bleh
         end
      end
      end_addr = end_addr + 1
   until end_addr == 0xFFFF
   while not is_significant(end_addr) do
      end_addr = end_addr - 1
   end
   ::bleh::
   init_records[#init_records+1] = "{\"base\":"..base_addr..",\"data\":\"base64:"..base64.encode(bin:sub(base_addr+1, end_addr+1)).."\"}"
   base_addr = end_addr + 1
end

local f = assert(io.open(tmplpath,"rb") or io.open("tests/standard.job.tmpl","rb"), "Couldn't open the template file *or* tests/standard.job.tmpl!")
local a = assert(f:read("*a"))
f:close()

a = a:gsub("%$%$%$%$", table.concat(init_records,","))
local f = assert(io.open(outpath,"wb"))
assert(f:write(a))
f:close()
