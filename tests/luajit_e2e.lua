print(jit.version, jit.os, jit.arch, 1 + 1, 7 / 2, 2 ^ 10, 10 % 3, -7 % 3, bit.lshift(1, 31), bit.bor(3, 4), bit.bnot(0))
print(math.huge, -math.huge, math.pi, math.sqrt(2), math.floor(-3.5))
print(string.format("%5.2f|%d|%s|%q|%x|%g|%10s|%-5d|", 3.14159, 42, "hi", "a\nb", 255, 1e20, "r", 7))
local t = {}
for i = 1, 100 do t[i] = (i * 7919) % 101 end
table.sort(t)
print(table.concat(t, ",", 1, 20), #t)
table.sort(t, function(a, b) return a > b end)
print(t[1], t[100])
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
print(fib(24))
local co = coroutine.wrap(function(a) local b = coroutine.yield(a + 1); local c = coroutine.yield(b * 2); return c .. "!" end)
print(co(1), co(10), co("x"))
local mt = setmetatable({}, {__index = function(_, k) return k * 2 end, __call = function(self, x) return x + 1 end,
  __add = function(a, b) return "add" end, __tostring = function() return "OBJ" end})
print(mt[21], mt(5), mt + 1, tostring(mt))
print(pcall(error, {code = 7}), pcall(error, "msg", 0), select("#", pcall(function() local x = nil; return x.y end)))
print(string.rep("ab", 5), ("hello world"):upper(), ("Hello"):byte(1, -1))
print(("a1b22c333"):gsub("%d+", function(d) return "<" .. #d .. ">" end))
print(string.find("the quick brown fox", "q(u)(i)"), ("x"):rep(0) == "")
for w in string.gmatch("one two  three", "%a+") do io.write(w, ";") end print()
print(tostring(1e15), tostring(2^53), 0.1 + 0.2, "10" + 5, 10 .. 20)
-- hot loops: the trace compiler records, optimizes, assembles and runs them
local s = 0
for i = 1, 2000000 do s = s + i % 7 end
print(s)
local x = 0.5
for i = 1, 1000000 do x = x * 1.0000001 + (i % 3) * 1e-9 end
print(string.format("%.10f", x))
local tab = {}
for i = 1, 200000 do tab[i] = i * 2 end
local acc = 0
for i = 1, #tab do acc = acc + tab[i] end
print(acc)
local str = {}
for i = 1, 3000 do str[#str + 1] = tostring(i) end
print(#table.concat(str), select(2, loadstring("return 1 +")))
-- ffi: C declarations, structs, arrays, callbacks into C, arithmetic on cdata
local ffi = require("ffi")
ffi.cdef[[
typedef struct { int x; double y; char tag[4]; } rec;
int abs(int);
size_t strlen(const char *);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
]]
local r = ffi.new("rec", {x = 5, y = 2.5})
r.tag = "ab"
print(r.x, r.y, ffi.string(r.tag), ffi.sizeof("rec"), ffi.offsetof("rec", "y"), ffi.C.abs(-9), ffi.C.strlen("hello"))
local arr = ffi.new("int[8]", 5, 3, 8, 1, 9, 2, 7, 4)
ffi.C.qsort(arr, 8, 4, function(a, b) return ffi.cast("int *", a)[0] - ffi.cast("int *", b)[0] end)
local o = {}
for i = 0, 7 do o[#o + 1] = arr[i] end
print(table.concat(o, " "))
local u = ffi.new("uint64_t", 1) * 3 + 4
print(tostring(u), tonumber(u), ffi.new("int64_t", -1) < 0)
local sum = 0
for i = 0, 7 do for j = 1, 20000 do sum = sum + arr[i] * j end end
print(sum)
print(pcall(function() return ffi.new("rec")[0].nope end))
-- string buffers and load
local f = loadstring("local a, b = ... return a * b, a .. b")
print(f(6, 7))
print(select("#", string.byte(string.rep("x", 1000), 1, -1)))
print(#string.dump(function() return 1 end) > 0, math.random and true)
