pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
function _init()
 -- Native PICO-8 syntax and fixed-point semantics, not a Lua text shim.
 local x=1
 x+=2
 assert(x==3 and 32767+1==-32768)
 assert(0xffff.ffff==-0x0.0001)
 assert(band(0x55,0x0f)==5)
 assert(shl(1,4)==16)
 local t={1,2}
 add(t,3)
 del(t,2)
 assert(#t==2 and t[2]==3)
 poke(0x4300,123)
 assert(peek(0x4300)==123)
 memcpy(0x4310,0x4300,1)
 assert(peek(0x4310)==123)
 cls(0)
 pset(5,7,8)
 assert(pget(5,7)==8)
 rectfill(10,10,12,12,9)
 assert(pget(12,12)==9)
 sset(0,0,11)
 spr(0,20,20)
 assert(pget(20,20)==11)
 mset(0,0,1)
 assert(mget(0,0)==1)
 sset(8,0,12)
 map(0,0,32,32,128,32)
 assert(pget(32,32)==12)
 camera(31,31)
 clip(1,1,1,1)
 map(0,0,32,32,128,32)
 assert(pget(32,32)==12)
 camera()
 clip()
 cartdata("dc_api_test")
 dset(0,42.5)
 assert(dget(0)==42.5)
 print("api checks passed",1,40,7)
end
function _update60() end
function _draw() end
