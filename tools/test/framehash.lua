-- framehash.lua: per-frame state hash for sync comparison between Gens builds.
-- Output: framehash.txt in the current directory: frame, Adler-32 of 68k RAM, Adler-32 of Z80 RAM, 68k regs.
local out = io.open("framehash.txt", "w")
local MAXFRAMES = tonumber(os.getenv("FRAMEHASH_MAX") or "") or 1000000
local function adler(t, n)
  local a, b = 1, 0
  for i = 1, n do
    a = (a + (t[i] or 0)) % 65521
    b = (b + a) % 65521
  end
  return b * 65536 + a
end
local regs = {"d0","d1","d2","d3","d4","d5","d6","d7","a0","a1","a2","a3","a4","a5","a6","a7","pc","sr"}
local done = false
emu.registerafter(function()
  if done then return end
  local f = emu.framecount()
  local ram = memory.readbyterange(0xFF0000, 0x10000)
  local z80 = memory.readbyterange(0xA00000, 0x2000)
  local line = string.format("%d %08X %08X", f, adler(ram, 0x10000), adler(z80, 0x2000))
  for _, r in ipairs(regs) do
    line = line .. string.format(" %08X", (memory.getregister("main." .. r) or 0) % 4294967296)
  end
  out:write(line, "\n")
  if f % 60 == 0 then out:flush() end
  if f >= MAXFRAMES then out:write("END\n"); out:close(); done = true end
end)
