pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- Dreamcast cartridge picker. Original code, MIT licensed.
function _init()
 carts=__listcarts()
 selected=1
 info=false
 status="five open-source samples"
 names={
  ["anteform.p8"]="anteform",
  ["bull_sheep.p8"]="bull sheep",
  ["elephant.p8"]="elephant in the room",
  ["picolumia.p8.png"]="picolumia",
  ["pikoralli.p8"]="pikoralli"
 }
end
function basename(path)
 local result=path
 for i=1,#path do
  if sub(path,i,i)=="/" then result=sub(path,i+1) end
 end
 return names[result] or result
end
function _update()
 if btnp(0) or btnp(1) then info=not info end
 if info then return end
 if #carts==0 then return end
 if btnp(2) then selected=(selected-2)%#carts+1 end
 if btnp(3) then selected=selected%#carts+1 end
 if btnp(4) or btnp(5) then load(carts[selected]) end
end
function _draw()
 cls(1)
 rectfill(0,0,127,27,0)
 print("dreamcast",6,5,6)
 print("pico-8 player",6,15,7)
 rectfill(108,7,119,18,8)
 rectfill(111,10,116,15,10)
 print(status,6,33,13)
 if info then
  print("freely licensed games",6,47,7)
  print("anteform - eric w. brown",6,58,6)
  print("pikoralli - ivan notaros",6,68,6)
  print("elephant - csaba ekart",6,78,6)
  print("bull sheep - abhijit kar",6,88,6)
  print("picolumia - andrew edstrom",6,98,6)
 else
 local start=max(1,selected-4)
 for i=start,min(#carts,start+5) do
  local y=46+(i-start)*10
  if i==selected then
   rectfill(3,y-2,124,y+6,2)
   print(">",6,y,10)
  end
  print(sub(basename(carts[i]),1,26),14,y,i==selected and 7 or 6)
 end
 end
 rectfill(0,110,127,127,0)
 print("a/b play  left/right info",5,113,7)
 print("l+r+start: back to games",5,121,13)
end
