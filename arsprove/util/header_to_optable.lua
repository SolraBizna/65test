#!/usr/bin/env lua5.3

local opcodes_seen = {}
local ops = {}

local ADDRESS_MODE_CODES = {
   ["<SuperImplied<>>"]="implied",
   ["<Implied<>>"]="implied",
   ["<ImpliedA<>>"]="impliedA",
   ["<ImpliedX<>>"]="impliedX",
   ["<ImpliedY<>>"]="impliedY",
   ["<Immediate<>>"]="immediate",
   ["<ZeroPageXIndirect<>>"]="zpxi",
   ["<ZeroPageIndirectY<>>"]="zpiy",
   ["<ZeroPageIndirectYBug<>>"]="zpiy",
   ["<ZeroPageX<>>"]="zpx",
   ["<ZeroPageY<>>"]="zpy",
   ["<ZeroPage<>>"]="zp",
   ["<ZeroPageIndirect<>>"]="zpi",
   ["<AbsoluteX<>>"]="absx",
   ["<AbsoluteXBug<>>"]="absx",
   ["<AbsoluteY<>>"]="absy",
   ["<AbsoluteYBug<>>"]="absy",
   ["<Absolute<>>"]="abs",
}

local ADDRESS_MODES = {}
for k,v in pairs(ADDRESS_MODE_CODES) do
   if not ADDRESS_MODES[v] then
      ADDRESS_MODES[v] = true
      ADDRESS_MODES[#ADDRESS_MODES+1] = v
   end
end
table.sort(ADDRESS_MODES)

local f = assert(io.open("include/w65c02.hh", "r"))
for l in f:lines() do
   local opcode, code
      = l:match("^          case 0x([0-9A-F][0-9A-F]): (.-); break;$")
   if opcode == nil then
      -- do nothing
   elseif opcodes_seen[opcode] then
      error("opcode "..opcode.." seen more than once")
   else
      code = code:gsub("AM::",""):gsub("Core<System>",""):gsub("%(%)$","")
      local op,rest = code:match("^([A-Za-z_][A-Za-z_0-9]*)(.*)$")
      if not op then
         error("Unparseable code: "..code)
      end
      op = op:gsub("_.*$","")
      opcodes_seen[code] = op
      if op == "JSR" or op == "BBS" or op == "BBR" or op == "SMB"
      or op == "RMB" or op == "Branch" or op == "JMP" then
         -- skip
      else
         local op_tab = ops[op]
         if not op_tab then
            op_tab = {}
            ops[op] = op_tab
            ops[#ops+1] = op
         end
         if rest == "" then rest = "<Implied<>>" end
         local mode = ADDRESS_MODE_CODES[rest]
         if mode then
            if op_tab[mode] then
               if op == "NOP" then
                  op_tab[mode] = op_tab[mode] .. "/" .. opcode
               else
                  error("Multiple "..mode.." for "..op)
               end
            else
               op_tab[mode] = opcode
            end
         else
            error("Unparseable address mode: "..rest)
         end
      end
   end
end
f:close()

local f = assert(io.open("gen/optable.csv", "w"))
table.sort(ops, function(a,b)
              if a == "NOP" then return false
              elseif b == "NOP" then return true
              else return a < b
              end
end)
f:write("Op")
for _,mode in ipairs(ADDRESS_MODES) do
   f:write(",",mode)
end
f:write("\n")
for _,opcode in ipairs(ops) do
   f:write(opcode)
   local op = ops[opcode]
   for _,mode in ipairs(ADDRESS_MODES) do
      f:write(",",op[mode] or "  ")
   end
   f:write("\n")
end
f:close()
