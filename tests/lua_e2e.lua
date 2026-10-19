print(_VERSION, 1 + 1, 7 // 2, 7 / 2, 2 ^ 10, 10 % 3, -7 // 2, -7 % 3, 1 << 62, 3 | 4, ~0)
print(math.maxinteger, math.mininteger, math.pi, math.huge, -math.huge, math.sqrt(2), math.floor(-3.5))
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
print(string.rep("ab", 5, "-"), ("hello world"):upper(), ("Hello"):byte(1, -1))
print(("a1b22c333"):gsub("%d+", function(d) return "<" .. #d .. ">" end))
print(string.find("the quick brown fox", "q(u)(i)"), ("x"):rep(0) == "")
for w in string.gmatch("one two  three", "%a+") do io.write(w, ";") end print()
print(tostring(1e15), tostring(2^53), 0.1 + 0.2, 100 // 1e0, 3 | 0, "10" + 5, 10 .. 20)
print(math.tointeger(3.0), math.type(1), math.type(1.0), math.type("1"), 5 // 0.0, -5 // 0.0, 0/0 ~= 0/0)
print(utf8.char(228, 8364, 128512), utf8.len("häl"), #utf8.char(128512))
print(os.time({year = 2020, month = 1, day = 1, hour = 12}), os.date("!%Y-%m-%d", 86400 * 365))
local s = 0
for i = 1, 1000000 do s = s + i % 7 end
print(s)
goto skip
print("not printed")
::skip::
local big = {}
for i = 1, 5000 do big[#big + 1] = tostring(i) end
print(#table.concat(big), select(2, load("return 1 +")))
print(string.pack("<i4", 100):byte(1, -1))
print(string.unpack("<d", string.pack("<d", 1.5)))
print(next({}), type(print), math.random and true)
print(("%s %s"):format(1, 2.5), tostring(nil), tostring(true), 2^63, math.ult(1, -1), 7 // -2, 7.5 // 2)
