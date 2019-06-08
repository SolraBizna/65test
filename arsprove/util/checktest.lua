#!/usr/bin/env lua5.3

local cjson = require "cjson"
local base64 = require "base64"

if #arg ~= 4 then
   print("usage: checktest leftname left.json rightname right.json")
   os.exit(1)
end

local INSTRUCTIONS = {[0]="BRK #xx","ORA(xx,X)","NOP xx","NOP","TSB xx","ORA xx","ASL xx","RMB0 xx","PHP","ORA #xx","ASL A","NOP","TSB xxxx","ORA xxxx","ASL xxxx","BBR0 xx,xx","BPL xx","ORA (xx),Y","ORA (xx)","NOP","TRB xx","ORA xx,X","ASL xx,X","RMB1 xx","CLC","ORA xxxx,Y","INC A","NOP","TRB xxxx","ORA xxxx,X","ASL xxxx,X","BBR1 xx,xx","JSR","AND(xx,X)","NOP xx","NOP","BIT xx","AND xx","ROL xx","RMB2 xx","PLP","AND #xx","ROL A","NOP","BIT xxxx","AND xxxx","ROL xxxx","BBR2 xx,xx","BMI xx","AND (xx),Y","AND (xx)","NOP","BIT xx,X","AND xx,X","ROL xx,X","RMB3 xx","SEC","AND xxxx,Y","DEC A","NOP","BIT xxxx,X","AND xxxx,X","ROL xxxx,X","BBR3 xx,xx","RTI","EOR(xx,X)","NOP xx","NOP","NOP xx","EOR xx","LSR xx","RMB4 xx","PHA","EOR #xx","LSR A","NOP","JMP xxxx","EOR xxxx","LSR xxxx","BBR4 xx,xx","BVC xx","EOR (xx),Y","EOR (xx)","NOP","NOP xx,X","EOR xx,X","LSR xx,X","RMB5 xx","CLI","EOR xxxx,Y","PHY","NOP","NOP xxxx","EOR xxxx,X","LSR xxxx,X","BBR5 xx,xx","RTS","ADC(xx,X)","NOP xx","NOP","STZ xx","ADC xx","ROR xx","RMB6 xx","PLA","ADC #xx","ROR A","NOP","JMP (xxxx)","ADC xxxx","ROR xxxx","BBR6 xx,xx","BVS xx","ADC (xx),Y","ADC (xx)","NOP","STZ xx,X","ADC xx,X","ROR xx,X","RMB7 xx","SEI","ADC xxxx,Y","PLY","NOP","JMP (xxxx,X)","ADC xxxx,X","ROR xxxx,X","BBR7 xx,xx","BRA","STA(xx,X)","NOP xx","NOP","STY xx","STA xx","STX xx","SMB0 xx","DEC Y","BIT #xx","TXA","NOP","STY xxxx","STA xxxx","STX xxxx","BBS0 xx,xx","BCC xx","STA (xx),Y","STA (xx)","NOP","STY xx,X","STA xx,X","STX xx,Y","SMB1 xx","TYA","STA xxxx,Y","TXS","NOP","STZ xxxx","STA xxxx,X","STZ xxxx,X","BBS1 xx,xx","LDY #xx","LDA(xx,X)","LDX #xx","NOP","LDY xx","LDA xx","LDX xx","SMB2 xx","TAY","LDA #xx","TAX","NOP","LDY xxxx","LDA xxxx","LDX xxxx","BBS2 xx,xx","BCS xx","LDA (xx),Y","LDA (xx)","NOP","LDY xx,X","LDA xx,X","LDX xx,Y","SMB3 xx","CLV","LDA xxxx,Y","TSX","NOP","LDY xxxx,X","LDA xxxx,X","LDX xxxx,Y","BBS3 xx,xx","CPY #xx","CMP(xx,X)","NOP #xx","NOP","CPY xx","CMP xx","DEC xx","SMB4 xx","INC Y","CMP #xx","DEC X","WAI","CPY xxxx","CMP xxxx","DEC xxxx","BBS4 xx,xx","BNE xx","CMP (xx),Y","CMP (xx)","NOP","NOP xx,X","CMP xx,X","DEC xx,X","SMB5 xx","CLD","CMP xxxx,Y","PHX","STP","NOP xxxx,X","CMP xxxx,X","DEC xxxx,X","BBS5 xx,xx","CPX #xx","SBC(xx,X)","NOP #xx","NOP","CPX xx","SBC xx","INC xx","SMB6 xx","INC X","SBC #xx","NOP","NOP","CPX xxxx","SBC xxxx","INC xxxx","BBS6 xx,xx","BEQ xx","SBC (xx),Y","SBC (xx)","NOP","NOP xx,X","SBC xx,X","INC xx,X","SMB7 xx","SED","SBC xxxx,Y","PLX","NOP","NOP xxxx,X","SBC xxxx,X","INC xxxx,X","BBS7 xx,xx"}

local leftname = arg[1]
local rightname = arg[3]

local f = assert(io.open(arg[2],"rb"))
local a = f:read("*a")
f:close()
local left = cjson.decode(a)
local f = assert(io.open(arg[4],"rb"))
local a = f:read("*a")
f:close()
local right = cjson.decode(a)

local pass = true

if left.num_cycles ~= right.num_cycles then
   print("Cycle counts differ.")
   print(("\t%7s: %i"):format(leftname, left.num_cycles))
   print(("\t%7s: %i"):format(rightname, right.num_cycles))
   pass = false
end
if left.last_pc ~= right.last_pc then
   print("Final PCs differ.")
   print(("\t%7s: $%04X"):format(leftname, left.last_pc))
   print(("\t%7s: $%04X"):format(rightname, right.last_pc))
   pass = false
end
if left.termination_cause ~= right.termination_cause then
   print("Termination causes differ.")
   print(("\t%7s: %s"):format(leftname, left.termination_cause))
   print(("\t%7s: %s"):format(rightname, right.termination_cause))
   pass = false
elseif #left.cycles > 0 and left.num_cycles > 1005 then
   print("Test runs for too long. (Ran for "..(left.num_cycles|0).." cycles)")
   pass = false
end
if left.serial_out_data == cjson.null then left.serial_out_data = nil end
if right.serial_out_data == cjson.null then right.serial_out_data = nil end
if left.serial_out_data ~= right.serial_out_data then
   print("Serial outputs differ.")
   if left.serial_out_data and right.serial_out_data
   and left.serial_out_data:sub(1,7) == "base64:"
   and right.serial_out_data:sub(1,7) == "base64:" then
      local ldec = assert(base64.decode(left.serial_out_data:sub(8,-1)))
      local rdec = assert(base64.decode(right.serial_out_data:sub(8,-1)))
      if #ldec ~= #rdec then
         print("Lengths differ: "..#ldec.." bytes vs. "..#rdec.." bytes")
      end
      local minlen = math.min(#ldec,#rdec)
      local first_differing_byte = nil
      for n=1,minlen do
         if ldec:sub(n,n) ~= rdec:sub(n,n) then
            first_differing_byte = n
            break
         end
      end
      if first_differing_byte then
         local start = first_differing_byte - (first_differing_byte % 16) - 32
         start = math.max(0, start)
         for n=start,math.min(minlen-1,start+24*16),16 do
            io.write(("%06X | "):format(n))
            for i=n+1,n+16 do
               if ldec:byte(i) ~= rdec:byte(i) then
                  io.write("\x1B[31;1m")
               end
               local q = ldec:byte(i)
               if q then
                  io.write(("%02X"):format(q))
               else
                  io.write("--")
               end
               if ldec:byte(i) ~= rdec:byte(i) then
                  io.write("\x1B[0m")
               end
            end
            io.write(" | ")
            for i=n+1,n+16 do
               if ldec:byte(i) ~= rdec:byte(i) then
                  io.write("\x1B[31;1m")
               end
               local q = rdec:byte(i)
               if q then
                  io.write(("%02X"):format(q))
               else
                  io.write("--")
               end
               if ldec:byte(i) ~= rdec:byte(i) then
                  io.write("\x1B[0m")
               end
            end
            io.write("\n")
         end
      end
   else
      local lshort,rshort
      if left.serial_out_data and #left.serial_out_data > 59 then
         lshort = left.serial_out_data:sub(1,57).."..."
      else
         lshort = left.serial_out_data
      end
      if right.serial_out_data and #right.serial_out_data > 59 then
         rshort = right.serial_out_data:sub(1,57).."..."
      else
         rshort = right.serial_out_data
      end
      print((("\t%7s: %q"):format(leftname, lshort):gsub("\n","n")))
      print((("\t%7s: %q"):format(rightname, rshort):gsub("\n","n")))
   end
   pass = false
end
if left.cycles or right.cycles then
   if left.cycles and not right.cycles then right.cycles = {} end
   if right.cycles and not left.cycles then left.cycles = {} end
   local identical_cycles = #left.cycles == #right.cycles
   local first_differing_cycle = math.min(#left.cycles, #right.cycles)
   for n=1,#left.cycles do
      if left.cycles[n] ~= right.cycles[n] then
         first_differing_cycle = n
         identical_cycles = false
         break
      end
   end
   if not identical_cycles then
      print("Reported bus traffic differs.")
      print(("\x1B[1m\t\t%7s - %7s\x1B[0m"):format(leftname, rightname))
      for n=math.max(1, first_differing_cycle - 10),math.min(math.max(#left.cycles, #right.cycles), first_differing_cycle + 20) do
         local parsed = left.cycles[n] and tonumber(left.cycles[n],16)
         if parsed and parsed & 0xF000000 == 0xF000000 then
            print(("\t\t($%02X = %s)"):format(parsed & 0xFF,
                                              INSTRUCTIONS[parsed & 0xFF]))
         end
         if left.cycles[n] ~= right.cycles[n] then
            io.write("\x1B[31;1m")
         end
         print(("\t%i\t%7s - %7s"):format(n+4,left.cycles[n] or "xxxxxxx",right.cycles[n] or "xxxxxxx"))
         if left.cycles[n] ~= right.cycles[n] then
            io.write("\x1B[0m")
         end
      end
      pass = false
   end
end

if not pass then
   os.exit(1)
end
