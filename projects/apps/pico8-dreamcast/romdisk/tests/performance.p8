pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
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
end
function _update60() end
function _draw() end
