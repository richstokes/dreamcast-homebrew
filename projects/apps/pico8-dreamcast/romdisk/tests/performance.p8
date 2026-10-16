pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- FAKE-08's assert() only prints and calls stop(), which the smoke test
-- cannot observe. Fail with a logged runtime error instead.
function assert(cond,msg)
 if not cond then
  printh("PICO8_TEST: FAIL ASSERT "..tostr(msg))
  local fail=nil
  return fail.assertion
 end
 return cond
end
function _init()
 -- Optimized FORLOOP must stop on signed fixed-point wraparound.
 local function loopcheck(a,b,s,want)
  local count=0
  for i=a,b,s do
   count+=1
   assert(count<100,"for loop wrapped")
  end
  assert(count==want,"for loop count")
 end
 loopcheck(32766,32767,1,2)
 loopcheck(-32767,-32768,-1,2)
 loopcheck(0x7fff.fffd,0x7fff.ffff,0x0.0001,3)
 loopcheck(0x8000.0002,0x8000.0000,-0x0.0001,3)
 loopcheck(-1,1,0.25,9)
 loopcheck(1,-1,-0.25,9)
 loopcheck(2,1,1,0)
 loopcheck(1,2,-1,0)
 loopcheck(0,32767,16384,2)
 loopcheck(0,-32768,-16384,3)
 assert(0x7fff.ffff+0x0.0001==-32768)
 assert(-32768-0x0.0001==0x7fff.ffff)
 assert(-32768/1==-32768)
 assert(0x1.8000*0x1.8000==0x2.4000)
 -- Calls, tail calls, table growth, closures, and coroutine yields after O3.
 local function factorial(n)
  if n==0 then return 1 end
  return n*factorial(n-1)
 end
 assert(factorial(7)==5040)
 local t={}
 for i=1,50 do t[i]=i*3 end
 foreach(t,function(v) assert(v%3==0) end)
 local c=cocreate(function() yield(13) return 29 end)
 local ok,v=coresume(c)
 assert(ok and v==13)
 ok,v=coresume(c)
 assert(ok and v==29)

 -- Interpreter fast paths (Dreamcast lvm.c). Exact 16.16 division with
 -- whole and fractional divisors, against the original 64-bit formula.
 local divcases={0x7fff.ffff,0x0002.0000,0x3fff.ffff,
  0x7fff.ffff,0xfffe.0000,0xc000.0001,0x7fff.ffff,0x0003.0000,0x2aaa.aaaa,
  0x7fff.ffff,0xfffd.0000,0xd555.5556,0x7fff.ffff,0x0007.0000,0x1249.2492,
  0x7fff.ffff,0x0080.0000,0x00ff.ffff,0x7fff.ffff,0xff80.0000,0xff00.0001,
  0x7fff.ffff,0x7fff.0000,0x0001.0002,0x7fff.ffff,0x8000.0000,0xffff.0001,
  0x7fff.ffff,0xffff.0000,0x8000.0001,0x7fff.ffff,0x0000.0000,0x7fff.ffff,
  0x8000.0000,0x0002.0000,0xc000.0000,0x8000.0000,0xfffe.0000,0x4000.0000,
  0x8000.0000,0x0003.0000,0xd555.5556,0x8000.0000,0xfffd.0000,0x2aaa.aaaa,
  0x8000.0000,0x0007.0000,0xedb6.db6e,0x8000.0000,0x0080.0000,0xff00.0000,
  0x8000.0000,0xff80.0000,0x0100.0000,0x8000.0000,0x7fff.0000,0xfffe.fffe,
  0x8000.0000,0x8000.0000,0x0001.0000,0x8000.0000,0xffff.0000,0x7fff.ffff,
  0x8000.0000,0x0000.0000,0x8000.0001,0xffff.ffff,0x0002.0000,0x0000.0000,
  0xffff.ffff,0xfffe.0000,0x0000.0000,0xffff.ffff,0x0003.0000,0x0000.0000,
  0xffff.ffff,0xfffd.0000,0x0000.0000,0xffff.ffff,0x0007.0000,0x0000.0000,
  0xffff.ffff,0x0080.0000,0x0000.0000,0xffff.ffff,0xff80.0000,0x0000.0000,
  0xffff.ffff,0x7fff.0000,0x0000.0000,0xffff.ffff,0x8000.0000,0x0000.0000,
  0xffff.ffff,0xffff.0000,0x0000.0001,0xffff.ffff,0x0000.0000,0x8000.0001,
  0x0000.0001,0x0002.0000,0x0000.0000,0x0000.0001,0xfffe.0000,0x0000.0000,
  0x0000.0001,0x0003.0000,0x0000.0000,0x0000.0001,0xfffd.0000,0x0000.0000,
  0x0000.0001,0x0007.0000,0x0000.0000,0x0000.0001,0x0080.0000,0x0000.0000,
  0x0000.0001,0xff80.0000,0x0000.0000,0x0000.0001,0x7fff.0000,0x0000.0000,
  0x0000.0001,0x8000.0000,0x0000.0000,0x0000.0001,0xffff.0000,0xffff.ffff,
  0x0000.0001,0x0000.0000,0x7fff.ffff,0x0005.8000,0x0002.0000,0x0002.c000,
  0x0005.8000,0xfffe.0000,0xfffd.4000,0x0005.8000,0x0003.0000,0x0001.d555,
  0x0005.8000,0xfffd.0000,0xfffe.2aab,0x0005.8000,0x0007.0000,0x0000.c924,
  0x0005.8000,0x0080.0000,0x0000.0b00,0x0005.8000,0xff80.0000,0xffff.f500,
  0x0005.8000,0x7fff.0000,0x0000.000b,0x0005.8000,0x8000.0000,0xffff.fff5,
  0x0005.8000,0xffff.0000,0xfffa.8000,0x0005.8000,0x0000.0000,0x7fff.ffff,
  0xfff9.0000,0x0002.0000,0xfffc.8000,0xfff9.0000,0xfffe.0000,0x0003.8000,
  0xfff9.0000,0x0003.0000,0xfffd.aaab,0xfff9.0000,0xfffd.0000,0x0002.5555,
  0xfff9.0000,0x0007.0000,0xffff.0000,0xfff9.0000,0x0080.0000,0xffff.f200,
  0xfff9.0000,0xff80.0000,0x0000.0e00,0xfff9.0000,0x7fff.0000,0xffff.fff2,
  0xfff9.0000,0x8000.0000,0x0000.000e,0xfff9.0000,0xffff.0000,0x0007.0000,
  0xfff9.0000,0x0000.0000,0x8000.0001,0x0012.3456,0x0002.0000,0x0009.1a2b,
  0x0012.3456,0xfffe.0000,0xfff6.e5d5,0x0012.3456,0x0003.0000,0x0006.1172,
  0x0012.3456,0xfffd.0000,0xfff9.ee8e,0x0012.3456,0x0007.0000,0x0002.99c3,
  0x0012.3456,0x0080.0000,0x0000.2468,0x0012.3456,0xff80.0000,0xffff.db98,
  0x0012.3456,0x7fff.0000,0x0000.0024,0x0012.3456,0x8000.0000,0xffff.ffdc,
  0x0012.3456,0xffff.0000,0xffed.cbaa,0x0012.3456,0x0000.0000,0x7fff.ffff,
  0xff9c.0000,0x0002.0000,0xffce.0000,0xff9c.0000,0xfffe.0000,0x0032.0000,
  0xff9c.0000,0x0003.0000,0xffde.aaab,0xff9c.0000,0xfffd.0000,0x0021.5555,
  0xff9c.0000,0x0007.0000,0xfff1.b6dc,0xff9c.0000,0x0080.0000,0xffff.3800,
  0xff9c.0000,0xff80.0000,0x0000.c800,0xff9c.0000,0x7fff.0000,0xffff.ff38,
  0xff9c.0000,0x8000.0000,0x0000.00c8,0xff9c.0000,0xffff.0000,0x0064.0000,
  0xff9c.0000,0x0000.0000,0x8000.0001,0x0003.ffff,0x0002.0000,0x0001.ffff,
  0x0003.ffff,0xfffe.0000,0xfffe.0001,0x0003.ffff,0x0003.0000,0x0001.5555,
  0x0003.ffff,0xfffd.0000,0xfffe.aaab,0x0003.ffff,0x0007.0000,0x0000.9249,
  0x0003.ffff,0x0080.0000,0x0000.07ff,0x0003.ffff,0xff80.0000,0xffff.f801,
  0x0003.ffff,0x7fff.0000,0x0000.0008,0x0003.ffff,0x8000.0000,0xffff.fff9,
  0x0003.ffff,0xffff.0000,0xfffc.0001,0x0003.ffff,0x0000.0000,0x7fff.ffff,
  0x0005.0000,0x0000.8000,0x000a.0000,0x0005.0000,0xfffe.8000,0xfffc.aaab,
  0x0005.0000,0x0000.0001,0x7fff.ffff,0xfff9.0000,0x0000.8000,0xfff2.0000,
  0xfff9.0000,0xfffe.8000,0x0004.aaaa,0xfff9.0000,0x0000.0001,0x8000.0001,
  0x7fff.ffff,0x0000.8000,0x7fff.ffff,0x7fff.ffff,0xfffe.8000,0xaaaa.aaac,
  0x7fff.ffff,0x0000.0001,0x7fff.ffff,}
 for i=1,#divcases,3 do
  local a,b,r=divcases[i],divcases[i+1],divcases[i+2]
  assert(a/b==r,"div "..tostr(a,true).."/"..tostr(b,true))
 end
 assert(-7%3==2 and 7%-3==1 and -7%-3==2 and 5.5%2==1.5 and 3%0==0)
 assert(-7\2==-4 and 7\2==3)
 -- Table reads and writes: array part, hash part, fractional and
 -- out-of-range number keys, existing nil slots, and metamethods.
 local a={10,20,30,x=1}
 assert(a[1]==10 and a[3]==30 and a[0]==nil and a[4]==nil and a[-1]==nil)
 assert(a[1.5]==nil and a.x==1 and a.y==nil and #a==3)
 a[1.5]=7 a[0]=5 a[-1]=4 a.y=2 a[4]=40
 assert(a[1.5]==7 and a[0]==5 and a[-1]==4 and a.y==2 and a[4]==40 and #a==4)
 a.x=nil a.x=3 assert(a.x==3)
 local log={}
 local mt={__index=function(t,k) return "i"..k end,
  __newindex=function(t,k,v) add(log,k) rawset(t,k,v) end,
  __len=function() return 99 end}
 local m=setmetatable({p=1},mt)
 assert(m.p==1 and m.q=="iq" and m[2]=="i2" and #m==99)
 m.p=2 m.q=3 m[1]=4 m.q=5
 assert(m.p==2 and m.q==5 and m[1]==4 and #log==2 and log[1]=="q" and log[2]==1)
 local base={hello=function(self) return self.n end}
 local obj=setmetatable({n=6},{__index=base})
 assert(obj:hello()==6)
 obj.hello=function() return 8 end
 assert(obj:hello()==8)
 local bad=cocreate(function() local t={} t[nil]=1 end)
 local ok,e=coresume(bad)
 assert(not ok and e and #tostr(e)>0,"nil table index")
 -- Globals, including the FAKE-08 sandbox fallback used when _ENV is local.
 assert(undefined_global_for_test==nil)
 newglobal_for_test=1 newglobal_for_test+=1
 assert(newglobal_for_test==2)
 local function envread()
  local _ENV={x=5}
  return x,circfill,missing_name
 end
 local ex,ecirc,emiss=envread()
 assert(ex==5 and ecirc==circfill and emiss==nil)
 setmetatable(_ENV,{__index=function(t,k) if k=="magic_for_test" then return 42 end end})
 assert(magic_for_test==42 and other_for_test==nil)
 setmetatable(_ENV,nil)
 -- Comparisons, including non-numbers and metamethods.
 assert(1<2 and not (2<1) and 2<=2 and -1<0 and 0x8000<0x7fff)
 assert("a"<"b" and "ab"<="ab" and 1==1 and 1~="1" and "x"=="x")
 local ca,cb={},{}
 local cmt={__eq=function() return true end,__lt=function() return true end,
  __le=function() return false end}
 setmetatable(ca,cmt) setmetatable(cb,cmt)
 assert(ca==cb and ca<cb and not (ca<=cb))
 ok=coresume(cocreate(function() return {}<1 end))
 assert(not ok,"compare table with number")
 -- Iterators run in the caller's VM invocation: all(), closures that yield,
 -- C iterators, and errors raised inside an iterator.
 local sum=0
 for v in all({1,2,3,4}) do sum+=v end
 assert(sum==10)
 local pairs_n=0
 for k,v in pairs({a=1,b=2,3}) do pairs_n+=v end
 assert(pairs_n==6)
 local function yielding_iter(n)
  local i=0
  return function()
   i+=1
   if i<=n then yield(i) return i end
  end
 end
 local co=cocreate(function()
  local s=0
  for v in yielding_iter(3) do s+=v*10 end
  return s
 end)
 local got={}
 while true do
  local ok,v=coresume(co)
  assert(ok,v)
  if costatus(co)=="dead" then assert(v==60) break end
  add(got,v)
 end
 assert(#got==3 and got[3]==3)
 local nested=0
 for x in all({1,2}) do for y in all({10,20}) do nested+=x*y end end
 assert(nested==90)
 ok,e=coresume(cocreate(function()
  for v in function() local boom=nil return boom.x end do end
 end))
 assert(not ok and sub(tostr(e),-43)=="attempt to index local 'boom' (a nil value)","iterator error "..tostr(e))
 -- Runtime errors report the line of the failing instruction, not of the
 -- last call (the VM saves its program counter before each error check).
 local function noop() end
 local function lineof(f)
  local ok,e=coresume(cocreate(f))
  assert(not ok,"expected an error")
  e=tostr(e)
  for i=1,#e do
   if sub(e,i,i)==":" then
    local j=i+1
    while j<=#e and ord(e,j)>=48 and ord(e,j)<=57 do j+=1 end
    if j>i+1 and sub(e,j,j)==":" then return tonum(sub(e,i+1,j-1)) end
   end
  end
  assert(false,"no line in "..e)
 end
 local l0=lineof(function() local r=nil return r.ref end)
 local l1=lineof(function() local t=nil noop()
  return t.x end)
 local l2=lineof(function() local t=nil noop()
  return t+1 end)
 local l3=lineof(function() local t={} noop()
  t[nil]=1 end)
 local l4=lineof(function() local t=nil noop()
  t() end)
 local l5=lineof(function() noop()
  for i=1,{} do end end)
 local l6=lineof(function() local t={} noop()
  return t<1 end)
 assert(l1==l0+2 and l2==l0+4 and l3==l0+6 and l4==l0+8 and l5==l0+10 and l6==l0+12,
  "error lines "..l0.." "..l1.." "..l2.." "..l3.." "..l4.." "..l5.." "..l6)
end
function _update60() end
function _draw() end
