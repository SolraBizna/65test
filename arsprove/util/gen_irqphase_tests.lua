#!/usr/bin/env lua5.3

local function generic_irqphase_test(name, body, base_cycle, depth)
   for phase=0,depth do
      local f = assert(io.open("tests/gen/irqphase/"..name.."_"..phase..".65c","wb"))
      assert(f:write(body))
      f:close()
      local f = assert(io.open("tests/gen/irqphase/"..name.."_"..phase..".job.tmpl","wb"))
      assert(f:write(('{"show_cycles":true,"max_cycles":50,"init":[$$$$],"terminate_on_bad_write":false,"irq":[%i]}'):format(base_cycle+phase)))
      f:close()
      local f = assert(io.open("tests/gen/irqphase/"..name.."_"..phase.."n.65c","wb"))
      assert(f:write(body))
      f:close()
      local f = assert(io.open("tests/gen/irqphase/"..name.."_"..phase.."n.job.tmpl","wb"))
      assert(f:write(('{"show_cycles":true,"max_cycles":50,"init":[$$$$],"terminate_on_bad_write":false,"nmi":[%i]}'):format(base_cycle+phase)))
      f:close()
   end
end

for opcode=0,255 do
   if (opcode&0x1F)==0x10 or (opcode&0xF)==0xF or opcode == 0x80 or opcode==0xCB then goto continue end
   local body = ([[
.INCLUDE "header.inc"
.DB %i

.ORGA $f000
rti:
	RTI
.ORGA $fffa
.DW rti
.ORGA $fffe
.DW rti
]]):format(opcode)
   local hex = ("%02X"):format(opcode)
   generic_irqphase_test(hex, body, 24, 8)
   ::continue::
end

