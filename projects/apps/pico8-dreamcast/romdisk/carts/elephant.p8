pico-8 cartridge // http://www.pico-8.com
version 38
__lua__
-- begin include/door.lua
function make_door()
	d={
		tx=get_tx(sprite_nums.vdoor),
		ty=get_ty(sprite_nums.vdoor),
		d='r' --direction
	}
	--door direction
	if current_lvl==n_lvls then
		return
	end
	tmp=get_all_tile_pos(sprite_nums.vdoor)
	tty=tmp[2].ty
	ttx=tmp[2].tx
	if tty==d.ty then
		--horizontal
		d.sprt=sprite_nums.hdoor
		if tty==1 then d.d='u' end
		if tty==8 then d.d='d' end
	end
	if ttx==d.tx then
		--vertical
		if ttx==1 then d.d='l' end
		if ttx==8 then d.d='r' end
	end
end

function draw_door()
	if not e.finish then
		if not is_door_horizontal() then
			spr(sprite_nums.vdoor-1,(d.tx-1)*16,(d.ty-1)*16,2,4,d.d=='l',false)
		else
			spr(sprite_nums.hdoor-16,(d.tx-1)*16,(d.ty-1)*16,4,2,false,d.d=='u') 
		end
	end
end

function is_door_horizontal()
	return d.d=='u' or d.d=='d'
end
-- end include/door.lua
-- begin include/elephant.lua
--elephant
function make_elephant()
	e={
		sprite=sprite_nums.elephant1,
		x=16,
		y=16,
		tx=get_tx(sprite_nums.elephant1),
		ty=get_ty(sprite_nums.elephant1),
		ntx=0,
		nty=0,
		w=31,
		h=31,
		f=0,
		stp=0,
		spd=1,
		d=0,
		nut_eat_c={col1=4,col2=9},
		w_drink_c={col1=12,col2=1},
		current_c={col1=4,col2=9},
		wall_break_time=0,
		wtx=0,
		wty=0,
		finish=false,
		hit_freeze=false,
		hit_freeze_time=40, -- meddig eszik egy mogyit
		hit_freeze_timer=0,
		should_move=false,
		seen_player=false,
		anim_speed=10,
		eyes_closed=false,
		last_horizontal_dir='r',
		vegtelen=0,
		ending_sound_played=false,
		scared=false,
		scared_time=20,
		scared_timer=0,
		scared_anim_played_up=false,
		scared_anim_played_down=false,
		scared_anim_played_left=false,
		scared_anim_played_right=false
	}
	e.x=(e.tx-1)*16
	e.y=(e.ty-1)*16
	e.ntx=e.tx
	e.nty=e.ty
	frame_counter=0
end

function draw_elephant()

	if e.scared then 
		e.step=0
		e.anim_speed=20 
	end

	--palette shift everything a shade darker
	if not e.hit_freeze then
		e.stp+=1
		if e.stp%e.anim_speed==0 then e.f+=1 end
		if e.f>1 then e.f=0 end
	end

	spr(e.sprite+e.f*4,e.x,e.y,4,4,e.last_horizontal_dir=='l',false)

	if not e.scared then draw_eyes() end
end

function draw_eyes()
	local _x=e.x
	local _y=e.y
	if e.last_horizontal_dir=='r' then
		if e.f==0 then
			rectfill( _x+24, _y+8, _x+27, _y+11,7) --szemfeherje
			rectfill( _x+26, _y+10, _x+27, _y+11,1) --pupilla
			if e.eyes_closed  then
				rectfill( _x+24, _y+6, _x+27, _y+11,6)
				rectfill( _x+24, _y+11, _x+27, _y+11,5)
			end
		else 
			rectfill( _x+24, _y+9, _x+27, _y+12,7)
			rectfill( _x+26, _y+11, _x+27, _y+12,1)
			if e.eyes_closed  then
				rectfill( _x+24, _y+7, _x+27, _y+12,6)
				rectfill( _x+24, _y+12, _x+27, _y+12,5)
			end
		end
	else
		if e.f==0 then
			rectfill( _x+4, _y+8, _x+7, _y+11,7) --szemfeherje
			rectfill( _x+4, _y+10, _x+5, _y+11,1) --pupilla
			if e.eyes_closed  then
				rectfill( _x+4, _y+6, _x+7, _y+11,6)
				rectfill( _x+4, _y+11, _x+7, _y+11,5)
			end
		else 
			rectfill( _x+4, _y+9, _x+7, _y+12,7) --szemfeherje
			rectfill( _x+4, _y+11, _x+5, _y+12,1) --pupilla
			if(e.eyes_closed) then
				rectfill( _x+4, _y+7, _x+7, _y+12,6) --szürkítés
				rectfill( _x+4, _y+12, _x+7, _y+12,5) --also vonal
			end
		end
	end
end

function update_elephant_d()

	frame_counter+=1
	if frame_counter%120<=5 then
		e.eyes_closed=true
	else
		e.eyes_closed=false
	end

	if e.wall_break_time>0 then
		spawnbrr((e.wtx-1)*16+8,(e.wty-1)*16-8,16,16,4,5,part)
		e.wall_break_time-=1
	end

	if e.hit_freeze or e.scared then
		return
	end

	if can_elephant_see_the_player() and (p.has_nut or is_on_tile(p.tx,p.ty, sprite_nums.peanut)) then
		return
	end

	if not e.seen_player then
		-- is in line with nut
		for _, n in ipairs(nuts) do
			if e.ty==n.ty or e.ty+1==n.ty then
				if can_see_through_x(n.tx,e.tx,n.ty) then
					if e.tx>n.tx then
						--elefant mogott
						e.should_move=true
						e.d='l'
						e.last_horizontal_dir='l'
					end
					if e.tx<n.tx then
						--elefant elott
						e.ntx=e.tx-1
						e.should_move=true
						e.d='r'
						e.last_horizontal_dir='r'
					end
				end
			end
			if e.tx==n.tx or e.tx+1==n.tx then
				if (can_see_through_y(n.ty,e.ty,n.tx)) then
					if e.ty>n.ty then
						--elefant felett
						e.should_move=true
						e.d='u'
					end
					if e.ty<n.ty then
						--elefant alatt
						e.should_move=true
						e.d='d'
					end
				end
			end
		end
	end

	if current_lvl==n_lvls then
		return
	end

	-- is in line with player
	if(e.ty==p.ty) or (e.ty+1==p.ty) then
		if (can_see_through_x(p.tx,e.tx,p.ty)) then
			if e.tx>p.tx then
				--elefant mogott
				e.seen_player=true
				e.d='r'
				e.last_horizontal_dir='r'
			end
			if (e.tx<p.tx) then
				--elefant elott
				e.seen_player=true
				e.d='l'
				e.last_horizontal_dir='l'
			end
		end
	end
	if(e.tx==p.tx) or (e.tx+1==p.tx) then
		if (can_see_through_y(p.ty,e.ty,p.tx)) then
			if (e.ty>p.ty) then
				--elefant felett
				e.seen_player=true
				e.d='d'
			end
			if (e.ty<p.ty) then
				--elefant alatt
				e.seen_player=true
				e.d='u'
			end
		end
	end
end

function ecollide_with_d()
	if (e.d=='r' and d.d=='r' and e.seen_player and is_tile_on_side(e.tx,e.ty,sprite_nums.vdoor,'r')) or
	   (e.d=='l' and d.d=='l' and e.seen_player and is_tile_on_side(e.tx+1,e.ty,sprite_nums.vdoor,'l')) or
	   (e.d=='u' and d.d=='u' and e.seen_player and is_tile_on_side(e.tx,e.ty+1,sprite_nums.vdoor,'u')) or
	   (e.d=='d' and d.d=='d' and e.seen_player and is_tile_on_side(e.tx,e.ty,sprite_nums.vdoor,'d'))
	then		
		e.finish=true
	end
end

function update_elephant() 
	ecollide_with_d()

	
	ecollide_with_objects(bwalls, ecollide_with_bwall)
	ecollide_with_objects(nuts, ecollide_with_nut)
	ecollide_with_objects(water, ecollide_with_water)
	ecollide_with_objects(traps, ecollide_with_trap)

	if e.hit_freeze then
		e.should_move=false
		--wait 10 frame
		if (e.last_horizontal_dir=='r') then
			spawnpukk(e.x+24,e.y+24,0,0,e.current_c.col1,e.current_c.col2,part)
		else
			spawnpukk(e.x+8,e.y+24,0,0,e.current_c.col1,e.current_c.col2,part)
		end
		if e.hit_freeze_timer<e.hit_freeze_time then
			e.hit_freeze_timer+=1
			return
		else
			e.hit_freeze=false
			e.hit_freeze_timer=0
		end
	end

	if e.scared then
		--e.sprite=e.scared_sprite
		--sfx(7)
		if e.scared_timer<e.scared_time then
			e.scared_timer+=1
			return
		else
			e.scared=false
			e.scared_timer=0
			e.sprite=sprite_nums.elephant1
		end
		return
	end

	if e.finish then
		if not e.ending_sound_played then
			e.ending_sound_played=true
			sfx(2)
		end
	end

	e.vegtelen+=1
	move_elephant()
	
	if not e.finish then
		if (e.x%tile_size==0 and e.y%tile_size==0) then
			e.tx=e.x/16+1
			e.ty=e.y/16+1
			e.ntx=e.tx
			e.nty=e.ty
			e.should_move=false
			--e.d=0
			update_elephant_d()
		else
			if (e.vegtelen%5==0) then
				sfx(0)
			end
			if (e.x>(e.tx-1)*16) then --jobbra megy és mid tile
				e.ntx=e.tx+1
			elseif (e.x<(e.tx-1)*16) then --balra megy és mid tile
				e.ntx=e.tx-1
			elseif (e.y>(e.ty-1)*16) then --lefele megy és mid tile
				e.nty=e.ty+1
			elseif (e.y<(e.ty-1)*16) then --felfele megy és mid tile
				e.nty=e.ty-1
			end

		end
	end
	
	-- vege ha elefant kier
	--if e.finish and (e.x>128
	--	or e.x+32<0 or e.y>128 
	--	or e.y+32<0) then
	--		finished=true
	--end
	updateparts(epart)
	
end

function update_scare(d,scared_anim_b)
    if e.d==d and ecan_move(d) and e.seen_player and not scared_anim_b then
		sfx(7)
		e.scared=true
		scared_anim_b=true --addig tru amig falhoz nem ér
		e.sprite=sprite_nums.scelephant1
	end
	if scared_anim_b and e.d==d and not ecan_move(d) then
		scared_anim_b=false --ujra meg tud ijedni?
	end
	return scared_anim_b
end

function move_elephant()

	e.anim_speed=10

	if (e.seen_player) then
		if (e.d=='r' and not ecan_move('r')) or
		   (e.d=='l' and not ecan_move('l')) or
		   (e.d=='u' and not ecan_move('u')) or
		   (e.d=='d' and not ecan_move('d'))
		then
			e.seen_player=false
		end
	end
	
	if (e.finish) then
		if (e.d=='r') then
			spawntrail(e.x,e.y+32,2,2,5,6,epart)
			e.x+=e.spd
		elseif (e.d=='l') then
			spawntrail(e.x+32,e.y+32,2,2,5,6,epart)
			e.x-=e.spd
		elseif (e.d=='u') then
			spawntrail(e.x+16,e.y+32,8,8,5,6,epart)
			e.y-=e.spd
		elseif (e.d=='d') then
			spawntrail(e.x+16,e.y,8,8,5,6,epart)
			e.y+=e.spd
		end
		return
	end
	
	e.scared_anim_played_left=update_scare('l',e.scared_anim_played_left)
	e.scared_anim_played_right=update_scare('r',e.scared_anim_played_right)
	e.scared_anim_played_up=update_scare('u',e.scared_anim_played_up)
	e.scared_anim_played_down=update_scare('d',e.scared_anim_played_down)

	if (e.d=='r') and ecan_move('r') and really_should_move()
		then --jobbra
		spawntrail(e.x,e.y+32,2,2,5,6,epart)
		e.x+=e.spd
		e.anim_speed=5
	elseif (e.d=='l') and ecan_move('l') and really_should_move()
		then --balra
			spawntrail(e.x+32,e.y+32,2,2,5,6,epart)
		e.x-=e.spd
		e.anim_speed=5
	elseif (e.d=='u') and ecan_move('u') and really_should_move()
		then --fel
			spawntrail(e.x+16,e.y+32,8,8,5,6,epart)
		e.y-=e.spd
		e.anim_speed=5
	elseif (e.d=='d') and ecan_move('d') and really_should_move()
		then --le
			spawntrail(e.x+16,e.y,8,8,5,6,epart)
		e.y+=e.spd
		e.anim_speed=5
	end
end

function eis_tile_on_side(letter,d)
	return (is_tile_on_side(e.tx,e.ty,letter,d)
		or is_tile_on_side(e.tx+1,e.ty,letter,d)
		or is_tile_on_side(e.tx,e.ty+1,letter,d)
		or is_tile_on_side(e.tx+1,e.ty+1,letter,d))
end

function eis_end_of_map(d)
	return (is_end_of_map(e.tx,e.ty,d) 
		or is_end_of_map(e.tx+1,e.ty,d) 
		or is_end_of_map(e.tx,e.ty+1,d) 
		or is_end_of_map(e.tx+1,e.ty+1,d))
end

function ecan_move(d)
	--d: direction 'l' 'r' 'u' 'd'
	if eis_end_of_map(d) or 
	   eis_tile_on_side(sprite_nums.player_top1,d) or
	   eis_tile_on_side(sprite_nums.wall,d) or
	   eis_tile_on_side(sprite_nums.hhole,d) or
	   eis_tile_on_side(sprite_nums.vhole,d) or
	   (eis_tile_on_side(sprite_nums.grid1,d) and not b.pressed) then
		return false
	end
	return true
end

function really_should_move()
	return e.should_move or e.seen_player
end
-- end include/elephant.lua
-- begin include/game_state.lua
--game state
function load_game_lvl(lvl)
	load_game_map(lvl)
	anim_timer=0
	finish_anim_timer=92
	tile_size=16
	d_op={[0]=1,0,3,2}
	make_gamemap(lvl)
	make_wall()
	make_nuts()
	make_player()
	make_elephant()
	make_door()
	make_water()
	make_bwall()
	init_button()
	init_grids()
	make_traps()
	make_mhc()
	make_h()
	make_carpet()
	make_cheeses()
	shake=0
	develop=0
	devspeed=0
	finished=false
	reading=false
	game_anim_speed=5
	game_over=false
	game_over_timer=60
	assisted_view=false
	room_text_timer=0
	tb_init(0,{help_texts[lvl]})
end

function init_game(lvl)
	_update = update_game
	_draw = draw_game
	current_lvl=lvl
	load_game_lvl(current_lvl)
end

function update_game()
	room_text_timer+=1
	if game_over then
		game_over_timer-=1
		if (game_over_timer<0) then
			load_game_lvl(current_lvl)
		end
	end

	if loaded() and not game_over then
		tb_update()
		update_player()
		if (p.first_move) then
			update_elephant()
			update_btraps()
		end
	end
	if not loaded() then
		anim_timer+=game_anim_speed
	end
	if (btnp(4)) then
		load_game_lvl(current_lvl)
	end
	if finished then
		finish_anim_timer-=game_anim_speed
		if (finish_anim_timer<0) then
			next_level()
		end
	end

	if (btnp(0,1)) then
		assisted_view=not assisted_view
	end

	
	updateparts(part)
end

function draw_game()
	cls()
	--map(0,0)
	draw_map()
	--spr(140,112,32,2,2) -- mogyoro
	--spr(64,16,16,4,4)   -- elefant
	draw_assist_view()
	draw_object(carpet)
	draw_objects(walls)
	draw_bwall()
	draw_objects(mhcs)
	draw_button()
	draw_objects(enuts)
	draw_objects(dwater)
	draw_btraps()
	drawparts(ppart)
	draw_objects(cheeses)
	draw_objects(eaten_cheeses)
	draw_objects(nuts)
	if not game_over then --ilyenkor a csapda van csak
		draw_player()
	end
	draw_grids()
	drawparts(epart)
	draw_traps()
	draw_elephant()
	draw_water()
	draw_h()
	draw_map_edge()
	draw_door()
	drawparts(part)

	draw_deadtrap()

	--draw_vision_border()

	--shade_unseen_tiles()

	--draw_assist_view()

	if loaded() then tb_draw() end 

	if room_text_timer<40 then
		print_current_map_number()
	end
		
	if not loaded() then
		load_anim(anim_timer)
	end

	if finished then
		load_anim(finish_anim_timer)
	end

	if semi_finish then
		semi_finish_timer-=1
		rectfill(32,48,96,64,1)
		obprint("your winner",42,52,7,0,2)
	end

	if semi_finish_timer < 0 then
		finished=true
	end
	doshake()
	
end

function load_anim(size,icx,icy)
	cx=icx or 64
	cy=icy or 64
	for i=90,size,-1 do
		circ(cx,cy,i,0)
		circ(cx+1,cy,i,0)
	end
	
	circ(cx,cy,size,7)
	circ(cx+1,cy,size,7)
end

function loaded()
	return anim_timer>=92
end

function switch_level(lvl)
	load_game_lvl(lvl)
end


function print_current_map_number()
	rectfill(32,48,96,64,1)
	if(current_lvl<10) then
		obprint("room "..current_lvl,42,52,7,0,2)
	else
		obprint("room "..current_lvl,38,52,7,0,2)
	end
end

function next_level()
	--itt kene lejatszani az uj animot
	current_lvl+=1
	if (current_lvl>n_lvls) then
		init_menu()
		return
	end
	switch_level(current_lvl)
end
-- end include/game_state.lua
-- begin include/grid_button.lua
--grid and button

function init_grids()
	grids=get_all_tile_pos(sprite_nums.grid1)
	gcsprite=138
	gosprite=170
end

function init_button()
	b = { 
		tx=get_tx(sprite_nums.button1), --gomb
		ty=get_ty(sprite_nums.button1),
		pressed=false,
		psprite=168,
		usprite=136
	}
end

function draw_button()
	if b.pressed then
		spr(b.psprite,(b.tx-1)*16,(b.ty-1)*16,2,2)
	else
		spr(b.usprite,(b.tx-1)*16,(b.ty-1)*16,2,2)
	end
end

function draw_grids()
	for _, g in ipairs(grids) do
		if(b.pressed) then
			spr(gosprite,(g.tx-1)*16,(g.ty-1)*16,2,2)
		else
			spr(gcsprite,(g.tx-1)*16,(g.ty-1)*16,2,2)
		end
	end
end

function press_button()
	if is_on_object(p.tx,p.ty,b) and not e_undergrid() then
		b.pressed= not b.pressed
		sfx(5)
	end
end

function e_undergrid()
	for _, g in ipairs(grids) do
		if (g.tx>=e.tx and g.tx<=e.tx+1) and (g.ty>=e.ty and g.ty<=e.ty+1)
			 or (g.tx>=e.ntx and g.tx<=e.ntx+1) and (g.ty>=e.nty and g.ty<=e.nty+1) then
			return true
		end
	end
	return false
end
-- end include/grid_button.lua
-- begin include/hole.lua
--hole
function make_h()
	hholes=get_all_tile_pos(sprite_nums.hhole)
	vholes=get_all_tile_pos(sprite_nums.vhole)
	local wvholes=get_all_tile_pos(sprite_nums.wvhole)
	local whholes=get_all_tile_pos(sprite_nums.whhole)

	table_concat(vholes,wvholes)
	table_concat(hholes,whholes)
end

function draw_h()
	draw_objects(hholes)
	draw_objects(vholes)
end



-- end include/hole.lua
-- begin include/main.lua
--main
function _init()
	init_menu()
end
-- end include/main.lua
-- begin include/manhole_cover.lua
--manholecover

function make_mhc()
	mhcs=get_all_tile_pos(sprite_nums.mhc)
end

-- end include/manhole_cover.lua
-- begin include/map.lua
function draw_tile(tx,ty)
	draw_colored_tile(tx,ty,COLORS.LIGHT_GREY,COLORS.LAVENDER,COLORS.LIGHT_PEACH,COLORS.DARK_BLUE)
end

function draw_map_edge()
	--poke(0x5f5f,0x10)
	--for i=0,15 do
	--	pal(i,i+128,2)
	--end
	--memset(0x5f78,0xff,8)
	local _x=0
	local _y=0
	local _col=1
	if d then
		local _x=(d.tx-1)*16
		local _y=(d.ty-1)*16
		draw_other_edges(d.d, _col)
		if d.d=='r' then 
			rectfill(126,0,127,_y,_col)--jobb
			rectfill(126,_y+32,127,127,_col)
		elseif d.d=='l' then
			rectfill(0,0,1,_y,_col)
			rectfill(0,_y+32,1,127,_col) 
		elseif d.d=='u' then
			rectfill(0,0,_x,1,_col)
			rectfill(_x+32,0,127,1,_col)
		elseif d.d=='d' then
			rectfill(0,126,_x,127,_col)
			rectfill(_x+32,126,127,127,_col)
		end

	else
		draw_other_edges('f',_col)
	end
end

function draw_other_edges(side, _col)
	local _sides={'l','r','u','d'}
	del(_sides,side)
	for s in all(_sides) do
		draw_edge(s, _col)
	end
end

function draw_edge(side, _col)
	if side=='u' then rectfill(0,0,127,1,_col) end  --fent
	if side=='d' then rectfill(0,126,127,127,_col) end--lent
	if side=='l' then rectfill(0,0,1,127,_col) end--bal
	if side=='r' then rectfill(126,0,127,127,_col) end--jobb
end

function draw_map()
	for i=1,8,1 do
		for j=1,8,1 do
			draw_tile(i,j)
		end
	end
end

function make_gamemap()
	gamemap=lvl_tmplt
end

function get_all_tile_pos(_sprnum)
	local t = {}
	for i = 1, 8 do
		for j = 1, 8 do
			if gamemap[i][j]==_sprnum then
				t[#t+1] = {tx=j, ty=i, sprt=_sprnum}
			end
		end
	end
	return t
end

function get_tx(_sprnum)
	for i = 1, 8 do
		for j = 1, 8 do
			if gamemap[i][j]==_sprnum then
				return j
			end
		end
 	end
	return 0
end

function get_ty(_sprnum)
	for i = 1, 8 do
		for j = 1, 8 do
			if gamemap[i][j]==_sprnum then
				return i
			end
 		end
	end
	return 0
end

local function has_value (tab, val)
    for index, value in ipairs(tab) do
        if value == val then
            return true
        end
    end

    return false
end

-- ty az y pozicio, a sor amiben vagunk, a tx1, tx2 peddig a két tile
function can_see_through_x(tx1,tx2,ty)
	local x1=min(tx1,tx2)
	local x2=max(tx1,tx2)
	for i=x1,x2 do
		if has_value(light_rigids,gamemap[ty][i]) then
			return false
		end
	end
	return true
end

function can_see_through_y(ty1,ty2,tx)
	local y1=min(ty1,ty2)
	local y2=max(ty1,ty2)
	for i=y1,y2 do
		if has_value(light_rigids,gamemap[i][tx]) then
			return false
		end
	end
	return true
end

function is_end_of_map(tx,ty,d)
	--d (direction): 'l', 'r', 'd', 'u'
	if (d=='l') then return tx==1 end
	if (d=='r') then return tx==8 end
	if (d=='d') then return ty==8 end
	if (d=='u') then return ty==1 end
end

function is_on_tile(tx,ty,letter)
	--nut
	if letter==sprite_nums.peanut then
		return is_on_objects(tx,ty,nuts)
	end
	--button
	if letter==sprite_nums.button1 then
		return is_on_object(tx,ty,b)
	end
	--grid
	if (letter==sprite_nums.grid1) then
		return is_on_objects(grids)
	end
	--door
	if letter==sprite_nums.vdoor then
		if (d.d=='l' or d.d=='r') then
			return tx==d.tx and (ty==d.ty or ty==d.ty+1)
		else
			return ty==d.ty and (tx==d.tx or tx==d.tx+1)
		end
	end
	
	--hole
	if letter==sprite_nums.hhole  then
		return gamemap[ty][tx]==letter or gamemap[ty][tx]==sprite_nums.whhole
	end
	if letter==sprite_nums.vhole then
		return gamemap[ty][tx]==letter or gamemap[ty][tx]==sprite_nums.wvhole
	end
end

function is_tile_on_side(tx,ty,letter,side)
	--letter: t for wall, w for water, m for nut, p for player, e for elephant, x for nothing, a for door
	
	--side: r for right, l for left, u for up, d for down
	--calculate side cell pos
	if (side=='r') then 
		stx=tx+1 
		sty=ty 
	elseif (side=='l') then 
		stx=tx-1 
		sty=ty 
	elseif (side=='u') then 
		stx=tx
		sty=ty-1
	elseif (side=='d') then 
		stx=tx
		sty=ty+1 
	end -- not possible

	--holes
	if letter==sprite_nums.hhole then
		if (gamemap[sty][stx]==letter or gamemap[sty][stx]==sprite_nums.whhole) then
			return true
		end
	end

	if letter==sprite_nums.vhole then
		if (gamemap[sty][stx]==letter or gamemap[sty][stx]==sprite_nums.wvhole) then
			return true
		end
	end
	
	-- brick or manholecover
	if (letter==sprite_nums.mhc) then 
		if (gamemap[sty][stx]==letter) then --since it cant be removed
			return true
		end
	end
	
	--wall
	if (letter==sprite_nums.wall) then 
		return is_on_objects(stx, sty, walls)
	end
	
	-- break wall
	if (letter==sprite_nums.bwall) then 
		return is_on_objects(stx, sty, bwalls)
	end
	
	-- water
	if (letter==sprite_nums.water1) then 
		return is_on_objects(stx, sty, water)
	end
	
	-- elephant
	if letter==sprite_nums.elephant1 then
		if e.finish then 
			return false 
		end
		if (side=='l' or side=='r') then
			if side=='l' then stx=tx-2 end
			if (ty==e.ty) or (ty-1==e.ty) or (ty==e.nty) or (ty-1==e.nty) then -- egy sorban vagy eggyel alatta
				if (stx==e.tx) or (stx==e.ntx) then
					return true
				end
			end
		end
		
		-- u vagy d
		if (side=='u' or side=='d') then
			if side=='u' then sty=ty-2 end
			if (tx==e.tx or tx-1==e.tx) or (tx==e.ntx) or (tx-1==e.ntx) then -- egy oszlopban vagy eggyel elotte
				if (sty==e.ty) or (sty==e.nty) then
					return true
				end
			end
		end
		return false
	end
	
	--door
	if letter==sprite_nums.vdoor then
		if(stx==d.tx) and (sty==d.ty) then
			return true
		end
	end
	
	--grid
	if (letter==sprite_nums.grid1) then
		return is_on_objects(stx, sty, grids)
	end
	
	--button
	if (letter==sprite_nums.button1) then
		return is_on_object(stx, sty, b)
	end
	
	return false
end
-- end include/map.lua
-- begin include/maps.lua
--map
n_lvls=29
sprite_nums={
	carpet=234,
	wall=128,
	bath_wall=204,
	bwall=160,
	bdwall=130,
	mhc=196,
	button1=136,
	button2=168,
	grid1=138,
	grid2=170,
	peanut=140,
	dpeanut=200,
	rpeanut=198,
	elephant1=64,
	elephant2=68,
	scelephant1=72,
	scelephant2=76,
	vdoor=143,
	hdoor=208,
	vhole=162,
	hhole=172,
	wvhole=206,
	whhole=238,
	cheese=14,
	water1=132,
	water2=164,
	dwater=202,
	trap_open=134,
	trap_closed1=166,
	trap_closed2=232,
	trap_closed3=224,
	dtrap=226,
	player_left1=0,
	player_left2=2,
	player_top1=32,
	player_top2=34,
	old_tv=8,
	new_tv=40,
	green1=10,
	green2=12,
	pink1=42,
	pink2=44,
	plant=4,
	dplant=6,
	wc=36,
	dwc=38,
	box=228,
	dbox=230,
	lamp=46
}

light_rigids={
	sprite_nums.wall,
	sprite_nums.bwall,
	sprite_nums.hhole,
	sprite_nums.vhole,
	sprite_nums.whhole,
	sprite_nums.hhole,
	sprite_nums.plant,
	sprite_nums.wc,
	sprite_nums.old_tv,
	sprite_nums.new_tv,
	sprite_nums.green1,
	sprite_nums.green2,
	sprite_nums.pink1,
	sprite_nums.pink2,
	sprite_nums.box,
	sprite_nums.lamp,
	sprite_nums.bath_wall
}

lvl_tmplt= {
	{'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'},
	{'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'},
	{'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'},
	{'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'},
	{'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'},
	{'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'},
	{'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'},
	{'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'}
}

function load_game_map(_lvl)
	local _colsx=8*((_lvl-1)%16)
	local _colsy=8*flr((_lvl-1)/16)
	for i = _colsx, _colsx+7, 1 do
		for j = _colsy, _colsy+7, 1 do
			--log(sprite_nums.vdoor..mget(i,j))
			local _sprite_num = mget(i,j)
			lvl_tmplt[j-_colsy+1][i-_colsx+1]=_sprite_num
		end
	end
end
-- end include/maps.lua
-- begin include/menu_state.lua

local text = "press ❎  to start"
local f = 0

function init_menu()
	clvl=1
	_update = update_menu
	_draw = draw_menu
	music(0)
	make_lvl_slclts()
	anim_timer2=92
	select_timer=120
	mission_selected=false
	anim_start=false
	menu_anim_speed=5
end

function update_menu()
	if (anim_start) then
		anim_timer2-=menu_anim_speed
	end
	if not mission_selected then
		if btnp(5) then
			--music(-1,700)
			sfx(5)
			mission_selected=true
			--anim_start=true
		end
		if btnp(1) then
			--switch level high
			sfx(7)
			menu_move('r')
		end
		if btnp(0) then
			--switch level low
			sfx(7)
			menu_move('l')
		end
	end
	f+=5
	
	if (anim_timer2<0) then
		init_game(clvl)
	end

	for _, l in ipairs(lvl_slcts) do
		if(l.nx>l.x) then
			--spawntrail(clvlslct.x+8,clvlslct.y+12,4,4,4,9)
			spawnpukk(clvlslct.x+8,clvlslct.y+8,0,0,4,9,part)
			l.x+=5
		elseif (l.nx<l.x) then
			spawnpukk(clvlslct.x+8,clvlslct.y+8,0,0,4,9,part)
			l.x-=5
		end
	end


	updateparts(part)

	if mission_selected and not anim_start then
		select_timer-=1
		if (select_timer<100) then
			anim_start=true
		end
	end
end

function menu_move(d)
		--d: l vagy r
		if (d=='r') then
			if clvl==n_lvls then return end
			clvl+=1
			for _, l in ipairs(lvl_slcts) do
	    		l.nx-=20
			end
		elseif (d=='l') then
			if clvl==1 then return end
			clvl-=1
			for _, l in ipairs(lvl_slcts) do
	    		l.nx+=20
			end
		end
end

function draw_menu()
	cls()
	pal()
	
	draw_map()
	rectfill(0,16,128,36,1)
	rectfill(0,56,128,68,0)
	rectfill(0,36,128,50,1)
	rectfill(0,80,128,110,0)
	obprint("elephant",19,20,9,0,3)
	bprint("in the room",22,38,9,2)
	--print("press ❎ to start",32,64,2)
	draw_lvl_slct()
	wavy_text("press ❎  to start",f)

	draw_map_edge()

	if mission_selected then
		if (f%50<=25) then
			--circfill(64,clvlslct.y+8,10,0)
			rectfill(clvlslct.x-3,clvlslct.y,clvlslct.x+20,clvlslct.y+26,0)
		end
	end
	
	if anim_start then
 		load_anim(anim_timer2)
	end

	drawparts(part)
	
end

clvlslct={
	x=56,
	y=82
}

function make_lvl_slclts()
	lvl_slcts={}
	for i=1,n_lvls do
		lvl_slcts[#lvl_slcts+1]={
			x=clvlslct.x+(i-1)*20,
			y=clvlslct.y+3,
			nx=clvlslct.x+(i-1)*20,
			ny=clvlslct.y+3,
			lvl=i,
			s=140
		}

	end
end


local a=0

function draw_lvl_slct()
	--circfill(lvlslct.x,lvlslct.y,5,7)
	--spr(140,lvlslct.x,lvlslct.y,2,2)
	a+=7
	
	for _, l in ipairs(lvl_slcts) do
		if (l.lvl==clvl) then
			rspr(102,clvlslct.x,clvlslct.y,a,2,2)
			--circ(64,clvlslct.y+8,10,7)
			--spr(l.s,clvlslct.x,clvlslct.y,2,2)
		else
			spr(l.s,l.x,l.y,2,2)
		end
		
	end
	
	--spr(140,clvlslct.x,clvlslct.y,2,2)
	--rspr(102,clvlslct.x,clvlslct.y,a,2,2)
	if (clvl<10) then
		print("room"..clvl,clvlslct.x-1,clvlslct.y+20,9)
	else
		print("room"..clvl,clvlslct.x-3,clvlslct.y+20,9)
	end
end
-- end include/menu_state.lua
-- begin include/nut.lua
--nuts

function make_nuts()
	nuts=get_all_tile_pos(sprite_nums.peanut)
	enuts={}
end

function pcollide_with_nut(n)
	if p.has_nut then return end
	p.has_nut=true
	del(nuts,n)
	sfx(6)
end

function ecollide_with_nut(n)
	enuts[#enuts+1] = {tx=n.tx, ty=n.ty, sprt=sprite_nums.dpeanut}
	del(nuts,n)
	--e.d=0
	e.hit_freeze=true
	e.current_c=e.nut_eat_c
	sfx(1)
end

function place_nut()
	if (p.has_nut) 
	and not is_on_objects(p.tx,p.ty,nuts) 
	and not is_on_objects(p.tx,p.ty,grids) 
	and not is_on_tile(p.tx,p.ty,sprite_nums.hhole) 
	and not is_on_tile(p.tx,p.ty,sprite_nums.vhole) 
	and not is_on_tile(p.tx,p.ty,sprite_nums.button1) then
		nuts[#nuts+1] = {tx=p.tx, ty=p.ty, sprt=sprite_nums.peanut}
		p.has_nut=false
	end
end

-- end include/nut.lua
-- begin include/player.lua
--player
function make_player()
	p={
		x=0,
		y=0,
		tx=get_tx(sprite_nums.player_top1),
		ty=get_ty(sprite_nums.player_top1),
		dx=0,
		dy=0,
		w=15,
		h=15,
		d=0,
		walk={[0]={0,2},{32,34},{1,1},{2,2}},
		t=0,
		f=1,
		stp=4,
		walking=false,
		spd=2,
		has_nut=false,
		first_move=false,
		hit_trap=false,
		vegtelen=0,
		cheese_eat_time=50
	}
	p.x=(p.tx-1)*16
	p.y=(p.ty-1)*16
end

function draw_player()
	if (p.walking) then anim_player() end

	--drawparts()
	
 palt(0,true)   --hide black
 --0 left, 1 right, 2 up, 3 down
 tmp=0
 if p.d>1 then
 	tmp=1
 end
 
 spr(p.walk[tmp][p.f],p.x,p.y,2,2,p.d==1,p.d==3)
	--draw nut on player
	if (p.has_nut) then
		spr(sprite_nums.peanut,p.x,p.y,2,2)
	end
end

function anim_player()
	p.t=(p.t+1)%p.stp
	if (p.t==0) p.f=p.f%#p.walk[p.d]+1
end

function update_player()
	pcollide_with_objects(traps, pcollide_with_trap)
	p.vegtelen+=1
	move_player()
	if (btnp(5)) then
		--handle nut taking
		if p.has_nut then
			place_nut()
		else
			pcollide_with_objects(nuts,pcollide_with_nut)
		end
		press_button()
		--this is where the button press should be
	end
	if (btn(5)) then 
		pcollide_with_objects(cheeses,pcollide_with_cheese) 
	end
	if (btnp(0) or btnp(1) or btnp(2) or btnp(3) or btnp(4)) then
		p.first_move=true
	end
	if p.walking then
		spawntrail(p.x+8,p.y+8,4,4,5,6,ppart)
		if (p.vegtelen%5==0) then
			sfx(4)
		end
	end
	updateparts(ppart)
end

function pcan_move(d)
	--d: direction 'l' 'r' 'u' 'd'
	local _x=p.tx
	local _y=p.ty
	if     is_end_of_map(_x,_y,d) or 
		   is_tile_on_side(_x,_y,sprite_nums.elephant1,d) or
		   is_tile_on_side(_x,_y,sprite_nums.water1,d) or
		   is_tile_on_side(_x,_y,sprite_nums.wall,d) or
		   is_tile_on_side(_x,_y,sprite_nums.bwall,d) or
		   is_tile_on_side(_x,_y,sprite_nums.mhc,d) or
		   (is_tile_on_side(_x,_y,sprite_nums.vhole,d) and (d=='r' or d=='l')) or
		   (is_tile_on_side(_x,_y,sprite_nums.hhole,d) and (d=='u' or d=='d')) or
		   is_on_tile(p.tx,p.ty,sprite_nums.vhole) and (d=='r' or d=='l') or
		   is_on_tile(p.tx,p.ty,sprite_nums.hhole) and (d=='u' or d=='d')
		then
		return false
	end
	return true
end

function move_player() 
 --if mid-tile...
 if (p.walking and (p.x%tile_size)>0 or (p.y%tile_size)>0) then
	if (btn(d_op[p.d])) then
		p.dx*=-1
		p.dy*=-1
		p.d=d_op[p.d]
	end     
 	--...keep 
 	p.x+=p.dx
 	p.y+=p.dy
       
 	--but if we get to a tile, stop
 	if (p.x%tile_size==0 and p.y%tile_size==0) then
  	p.dx=0
  	p.dy=0
  	p.walking=false
  	p.t,p.f=0,1 --reset anim vars
 		--gamemap[p.ty][p.tx]='x'
	 	p.tx=p.x/tile_size+1
	 	p.ty=p.y/tile_size+1
	 	if (e.finish and is_on_tile(p.tx,p.ty,sprite_nums.vdoor)) then
	 		--win
	 		--next_level()
			finished=true
	 		--p.tx=1
	 		--p.x=(p.tx-1)*16
	 	end
 		--gamemap[p.ty][p.tx]=sprite_nums.player_top1
 	end
 	--but if on a tile, allow new input
		else
  	if (btn(0)) then
   	p.d=0
   	if pcan_move('l') then
   		p.walking=true
   		p.dx=-p.spd
   	end
  	elseif (btn(1)) then
   	p.d=1
    if pcan_move('r') then
   		p.walking=true
   		p.dx=p.spd
   	end
  	elseif (btn(2))
   then
   	p.d=2
   	if pcan_move('u') then
   		p.walking=true
   		p.dy=-p.spd
   	end
  	elseif (btn(3)) then
   	p.d=3
   	if pcan_move('d') then
   		p.walking=true
   		p.dy=p.spd
   	end
   end
  

  if (btn()>0) then
   p.x+=p.dx
   p.y+=p.dy
  else
   p.dx=0
  	p.dy=0
 	end
	end
end

function can_elephant_see_the_player()
	-- is in line with player
	if(e.ty==p.ty) or (e.ty+1==p.ty) then
		if (can_see_through_x(p.tx,e.tx,p.ty)) then
			if (e.tx>p.tx) then
				return true
			end
			if (e.tx<p.tx) then
				return true
			end
		end
	end
	if(e.tx==p.tx) or (e.tx+1==p.tx) then
		if (can_see_through_y(p.ty,e.ty,p.tx)) then
			if (e.ty>p.ty) then
				return true
			end
			if (e.ty<p.ty) then
				return true
			end
		end
	end
	return false
end
-- end include/player.lua
-- begin include/third_party.lua
--third parties
function bprint(str,x,y,c,scale)
	_str_to_sprite_sheet(str)
	
	local w = #str*4
	local h = 5
	
	pal(7,c)
	palt(0,true)
	
	sspr(0,0,w,h,x,y,w*scale,h*scale)
	pal()
	
	_restore_sprites_from_usermem()
end

function obprint(str,x,y,c,co,scale)
	_str_to_sprite_sheet(str)
	
	local w = #str*4
	local h = 5
	palt(0,true)
	
	pal(7,co)
	
	
	for xx=-2,1,2 do
		for yy=-2,1,2 do
			sspr(0,0,w,h,x+xx,y+yy,w*scale,h*scale)
		end
	end
	
	pal(7,c)
	sspr(0,0,w,h,x,y,w*scale,h*scale)
	pal()
	
	_restore_sprites_from_usermem()
end

function _str_to_sprite_sheet(str)
	_copy_sprites_to_usermem()
	_black_out_sprite_row()
	set_sprite_target()
	print(str,0,0,7)
	set_screen_target()
end

function set_sprite_target()
	poke(0x5f55,0x00)
end

function set_screen_target()
	poke(0x5f55,0x60)
end

function _copy_sprites_to_usermem()
	memcpy(0x4300,0x0,0x0200)
end

function _black_out_sprite_row()
	memset(0x0,0,0x0200)
end

function _restore_sprites_from_usermem()
	memcpy(0x0,0x4300,0x0200)
end


--wavy text
function wavy_text(text,f)
	local y
 local c
 local x = 128/2 - (#text*4)/2
 for c=1,#text do
 	y = sin((x+f)/100) * 2
  color(5)
  print(sub(text,c,c),x,(64-4)+y)
  y = sin((x+10+f)/100) * 2
  color(7)
  print(sub(text,c,c),x,(64-4)+y)
  x = x+4
 end
end


--copy table
--function deepcopy(orig)
--    local orig_type = type(orig)
--    local copy
--    if orig_type == 'table' then
--        copy = {}
--        for orig_key, orig_value in next, orig, nil do
--            copy[deepcopy(orig_key)] = deepcopy(orig_value)
--        end
--        setmetatable(copy, deepcopy(getmetatable(orig)))
--    else -- number, string, boolean, etc
--        copy = orig
--    end
--    return copy
--end

--rotating sprite
function rspr(s,x,y,a,w,h)
 sw=(w or 1)*8
 sh=(h or 1)*8
 sx=(s%8)*8
 sy=flr(s/8)*8
 x0=flr(0.5*sw)
 y0=flr(0.5*sh)
 a=a/360
 sa=sin(a)
 ca=cos(a)
 for ix=sw*-1,sw+4 do
  for iy=sh*-1,sh+4 do
   dx=ix-x0
   dy=iy-y0
   xx=flr(dx*ca-dy*sa+x0)
   yy=flr(dx*sa+dy*ca+y0)
   if (xx>=0 and xx<sw and yy>=0 and yy<=sh-1) then
    local col = sget(sx+xx,sy+yy)
		if col != 0 then
			pset(x+ix,y+iy,col)
		end
   end
  end
 end
end

function doshake()
	-- this function does the
	-- shaking
	-- first we generate two
	-- random numbers between
	-- -16 and +16
	local shakex=16-rnd(32)
	local shakey=16-rnd(32)

	-- then we apply the shake
	-- strength
	shakex*=shake
	shakey*=shake

	-- then we move the camera
	-- this means that everything
	-- you draw on the screen
	-- afterwards will be shifted
	-- by that many pixels
	camera(shakex,shakey)

	-- finally, fade out the shake
	-- reset to 0 when very low
	shake = shake*0.95
	if (shake<0.05) shake=0
	end

	function fadepal(_perc)
	-- this function sets the
	-- color palette so everything
	-- you draw afterwards will
	-- appear darker
	-- it accepts a number from
	-- 0 means normal
	-- 1 is completely black
	-- this function has been
	-- adapted from the jelpi.p8
	-- demo

	-- first we take our argument
	-- and turn it into a 
	-- percentage number (0-100)
	-- also making sure its not
	-- out of bounds  
	local p=flr(mid(0,_perc,1)*100)

	-- these are helper variables
	local kmax,col,dpal,j,k

	-- this is a table to do the
	-- palette shifiting. it tells
	-- what number changes into
	-- what when it gets darker
	-- so number 
	-- 15 becomes 14
	-- 14 becomes 13
	-- 13 becomes 1
	-- 12 becomes 3
	-- etc...
	dpal={0,1,1, 2,1,13,6,
			4,4,9,3, 13,1,13,14}

	-- now we go trough all colors
	for j=1,15 do
	--grab the current color
	col = j

	--now calculate how many
	--times we want to fade the
	--color.
	--this is a messy formula
	--and not exact science.
	--but basically when kmax
	--reaches 5 every color gets 
	--turns black.
	kmax=(p+(j*1.46))/22

	--now we send the color 
	--through our table kmax
	--times to derive the final
	--color
	for k=1,kmax do
		col=dpal[col]
	end

	--finally, we change the
	--palette
	pal(j,col)
	end
end

function table_concat(t1,t2) --feltetelezzuk hogy object
    for i=1,#t2 do
        t1[#t1+1] = {tx=t2[i].tx, ty=t2[i].ty, sprt=t2[i].sprt}
    end
    return t1
end
-- end include/third_party.lua
-- begin include/wall.lua
--wall
function make_wall()
	walls=get_all_tile_pos(sprite_nums.wall)
	local pinks1=get_all_tile_pos(sprite_nums.pink1)
	local pinks2=get_all_tile_pos(sprite_nums.pink2)
	local greens1=get_all_tile_pos(sprite_nums.green1)
	local greens2=get_all_tile_pos(sprite_nums.green2)
	local oldtvs=get_all_tile_pos(sprite_nums.old_tv)
	local newtvs=get_all_tile_pos(sprite_nums.new_tv)
	local lamps=get_all_tile_pos(sprite_nums.lamp)
	local bath_wall=get_all_tile_pos(sprite_nums.bath_wall)

	table_concat(walls,pinks1)
	table_concat(walls,pinks2)
	table_concat(walls,greens1)
	table_concat(walls,greens2)
	table_concat(walls,oldtvs)
	table_concat(walls,newtvs)
	table_concat(walls,lamps)
	table_concat(walls,bath_wall)
end

--breakable wall
function make_bwall()
	bwalls=get_all_tile_pos(sprite_nums.bwall)
	local plants=get_all_tile_pos(sprite_nums.plant)
	local wcs=get_all_tile_pos(sprite_nums.wc)
	local boxes=get_all_tile_pos(sprite_nums.box)

	table_concat(bwalls,plants)
	table_concat(bwalls,wcs)
	table_concat(bwalls,boxes)
	bdwalls={}
end

function draw_bwall()
	draw_objects(bwalls)
	draw_objects(bdwalls)
end

function ecollide_with_bwall(bw)
	shake+=0.1
	devspeed+=0.01
	e.wall_break_time=20
	e.wtx=bw.tx
	e.wty=bw.ty
	bdwalls[#bdwalls+1] = {tx=bw.tx, ty=bw.ty, sprt=get_destroyed_sprt(bw.sprt)}
	gamemap[bw.ty][bw.tx]='x'
	del(bwalls,bw)
end

function get_destroyed_sprt(_sprnum)
	if _sprnum == sprite_nums.bwall then return sprite_nums.bdwall end
	if _sprnum == sprite_nums.plant then return sprite_nums.dplant end
	if _sprnum == sprite_nums.wc then return sprite_nums.dwc end
	if _sprnum == sprite_nums.box then return sprite_nums.dbox end
end
-- end include/wall.lua
-- begin include/water.lua
--water
function make_water()
	water=get_all_tile_pos(sprite_nums.water1)
	dwater={}
	wtrsprite=132 --164
	dwtrsprite=202
	w_anim={
		f=0,
		stp=0,
	}
end

function draw_water()
	w_anim.stp+=1
	if(w_anim.stp%10==0) then w_anim.f+=1 end
	if(w_anim.f>1) then w_anim.f=0 end

	for _, w in ipairs(water) do
		spr(wtrsprite+w_anim.f*32,(w.tx-1)*16,(w.ty-1)*16,2,2)
	end
end

function ecollide_with_water(w)
	dwater[#dwater+1] = {tx=w.tx, ty=w.ty, sprt=sprite_nums.dwater} -- add to deleted waters
	del(water,w)
	--e.d=0
	e.current_c=e.w_drink_c
	e.hit_freeze=true
	sfx(8)
end



-- end include/water.lua
-- begin include/particles.lua

epart={} --elephant particles
ppart={} --player particles
part={} --general particles

function addpart(_x,_y,_dx,_dy,_g,_type,_maxage,_col,_oldcol,_part)
    local _p={}
    _p.x=_x --x coord
    _p.y=_y -- y coord
    _p.tpe=_type -- no use yet
    _p.mage=_maxage --max age of part
    _p.age=0 --current age of part
    _p.col=_col --first color
    _p.oldcol=_oldcol --fading out color
    _p.dx=_dx --starting dx
    _p.dy=_dy --starting dy
    _p.g=_g --gravity
    add(_part,_p)
end

function spawnpukk(_x,_y,_sx,_sy,_col,_oldcol,_part)
    --local _ang = rnd()
    local _ox = sin(_ang)*_sx
    local _oy = cos(_ang)*_sy
    local _dx = rnd(2.5)-1.25
    local _dy = -rnd(2)
    local _i = 30
    local _g = 0.2
    local _mage = 6+rnd(5)
    if (rnd(100)<=_i) then
        addpart(_x+_ox,_y+_oy,_dx,_dy,_g,0,_mage,_col,_oldcol,_part)
    end
end

function spawnbrr(_x,_y,_sx,_sy,_col,_oldcol,_part)
    for i=1,5,1 do
        --local _ang = rnd()
        local _ox = sin(_ang)*_sx
        local _oy = cos(_ang)*_sy
        local _dx = rnd(2.5)-1.25
        local _dy = -rnd(2)
        local _g = 0.2
        local _mage = 15+rnd(5)
    
        addpart(_x+_ox,_y+_oy,_dx,_dy,_g,0,_mage,_col,_oldcol,_part)
    end
end


function spawntrail(_x,_y,_sx,_sy,_col,_oldcol,_part)
    for i=1,2,1 do
    local _ang = rnd()
    local _ox = sin(_ang)*_sx
    local _oy = cos(_ang)*_sy
    
    addpart(_x+_ox,_y+_oy,0,0,0,0,5+rnd(5),_col,_oldcol,_part)
    end
end

function updateparts(_part)
    local _p
    for i=#_part,1,-1 do
        _p=_part[i]
        _p.dy+=_p.g
        _p.y+=_p.dy
        _p.x+=_p.dx
        _p.age+=1
        if _p.age>_p.mage then
            del(_part,_part[i])
        else
            if (_p.age/_p.mage)>0.5 then
                _p.col=_p.oldcol
            end
        end
    end
end

function drawparts(_part)
    for i=1,#_part do
        _p=_part[i]
        if _p.tpe==0 then
            pset(_p.x,_p.y,_p.col)
        end
    end
end
-- end include/particles.lua
-- begin include/textbox.lua
--- textbox: https://www.lexaloffle.com/bbs/?tid=38668
function tb_init(voice,string) -- this function starts and defines a text box.
    reading=true -- sets reading to true when a text box has been called.
    tb={ -- table containing all properties of a text box. i like to work with tables, but you could use global variables if you preffer.
        str=string, -- the strings. remember: this is the table of strings you passed to this function when you called on _update()
        voice=voice, -- the voice. again, this was passed to this function when you called it on _update()
        i=1, -- index used to tell what string from tb.str to read.
        cur=0, -- buffer used to progressively show characters on the text box.
        char=0, -- current character to be drawn on the text box.
        x=0, -- x coordinate
        y=5, -- y coordginate (106 default)
        w=127, -- text box width
        h=21, -- text box height
        col1=0, -- background color
        col2=7, -- border color
        col3=7, -- text color
        time=200 -- until its dismissed automatically
    }
end

function tb_update()  -- this function handles the text box on every frame update.
    tb.time-=1
    if tb.char<#tb.str[tb.i] then -- if the message has not been processed until it's last character:
        tb.cur+=0.8 -- increase the buffer. 0.5 is already max speed for this setup. if you want messages to show slower, set this to a lower number. this should not be lower than 0.1 and also should not be higher than 0.9
        if tb.cur>0.9 then -- if the buffer is larger than 0.9:
            tb.char+=1 -- set next character to be drawn.
            tb.cur=0    -- reset the buffer.
            --if (ord(tb.str[tb.i],tb.char)!=32) sfx(tb.voice) -- play the voice sound effect.
        end
        if (btnp(5)) tb.char=#tb.str[tb.i] -- advance to the last character, to speed up the message.
    elseif btnp(5) then -- if already on the last message character and button ❎/x is pressed:
        if #tb.str>tb.i then -- if the number of strings to disay is larger than the current index (this means that there's another message to display next):
            tb.i+=1 -- increase the index, to display the next message on tb.str
            tb.cur=0 -- reset the buffer.
            tb.char=0 -- reset the character position.
        else -- if there are no more mesages to display:
            reading=false -- set reading to false. this makes sure the text box isn't drawn on screen and can be used to resume normal gameplay.
        end
    end
    if (tb.time<0) then
        reading=false
    end
end

function tb_draw() -- this function draws the text box.
    local _wat = help_texts[current_lvl]==""
    if reading and not _wat then -- only draw the text box if reading is true, that is, if a text box has been called and tb_init() has already happened.
        rectfill(tb.x,tb.y,tb.x+tb.w,tb.y+tb.h,tb.col1) -- draw the background.
        rect(tb.x,tb.y,tb.x+tb.w,tb.y+tb.h,tb.col2) -- draw the border.
        print(sub(tb.str[tb.i],1,tb.char),tb.x+2,tb.y+2,tb.col3) -- draw the text.
    end
end
-- end include/textbox.lua
-- begin include/shading.lua
COLORS={
    BLACK=0,
    DARK_BLUE=1,
    DARK_PURPLE=2,
    DARK_GREEN=3,
    BROWN=4,
    DARK_GREY=5,
    LIGHT_GREY=6,
    WHITE=7,
    RED=8,
    ORANGE=9,
    YELLOW=10,
    GREEN=11,
    BLUE=12,
    LAVENDER=13,
    PINK=14,
    LIGHT_PEACH=15
}

function draw_assist_view()
    if assisted_view then
        for tx=1,8,1 do
            for ty=1,8,1 do
                local _seen=false
                if(e.ty==ty) or (e.ty+1==ty) then
                    if (can_see_through_x(tx,e.tx,ty)) then
                        _seen=true
                    end
                end
                if(e.tx==tx) or (e.tx+1==tx) then
                    if (can_see_through_y(ty,e.ty,tx)) then
                        _seen=true
                    end
                end
                if _seen then
                    --shade_tile(tx,ty)
                    if (can_elephant_see_the_player()) then
                        draw_colored_tile(tx,ty,COLORS.LIGHT_GREY,COLORS.DARK_PURPLE,COLORS.RED,COLORS.DARK_BLUE)
                    else
                        draw_colored_tile(tx,ty,COLORS.LIGHT_GREY,COLORS.DARK_GREEN,COLORS.GREEN,COLORS.DARK_BLUE)
                    end
                    if (can_elephant_see_the_player() and (p.has_nut or is_on_tile(p.tx,p.ty, sprite_nums.peanut))) then
                        draw_colored_tile(tx,ty,COLORS.LIGHT_GREY,COLORS.ORANGE,COLORS.YELLOW,COLORS.DARK_BLUE)
                    end
                end
            end
        end
    end
end

function draw_colored_tile(tx,ty,_col1,_col2,_col3,_col4)
	local _x1=(tx-1)*16
	local _y1=(ty-1)*16
	local _x2=tx*16
	local _y2=ty*16
	rectfill(_x1  ,_y1  ,_x2  ,_y2,_col1) --  4 sarok
	rectfill(_x1  ,_y1+1,_x2  ,_y2-1,_col3) -- háttér
	rectfill(_x1+1,_y1  ,_x2-1,_y2,_col3) -- háttér
	rectfill(_x1+1,_y1+1,_x2-1,_y2-1,_col4) -- 4 sötétkék pötty
	rectfill(_x1+2,_y1+1,_x2-2,_y2-1,_col3)  --8 háttérszínű geci megint
	rectfill(_x1+1,_y1+2,_x2-1,_y2-2,_col3) --8 háttérszínű geci megint
	rectfill(_x1+1,_y1+3,_x2-1,_y2-3,_col2) --lila outline
	rectfill(_x1+2,_y1+2,_x2-2,_y2-2,_col2)
	rectfill(_x1+3,_y1+1,_x2-3,_y2-1,_col2)

	rectfill(_x1+2,_y1+4,_x2-2,_y2-4,_col3) --belső háttér
	rectfill(_x1+3,_y1+3,_x2-3,_y2-3,_col3)
	rectfill(_x1+4,_y1+2,_x2-4,_y2-2,_col3)
end
-- end include/shading.lua
-- begin include/trap.lua

function make_traps()
    traps=get_all_tile_pos(sprite_nums.trap_open)
    btraps={} --broken traps
    dead_trap={
        tx=0,
        ty=0,
        f=0,
        spr1=166,
        spr2=232,
        spr3=224,
        stp=0
    }
	trap_sprite=134
    btrap_sprite=226
    destroy_time=30
    dead_sound_played=false
end

function draw_traps()
    draw_objects(traps)
end

function draw_btraps()
    draw_objects(btraps)
end

function update_btraps()
    for _, bt in ipairs(btraps) do
        if (bt.dt>0) then
            bt.dt-=1
            spawnpukk((bt.tx-1)*16+8,(bt.ty-1)*16+8,0,0,e.current_c.col1,e.current_c.col2,part)
        end
    end
end

function draw_deadtrap()
    if not game_over then
        return
    end

    dead_trap.stp+=1
	if(dead_trap.stp%10==0) then dead_trap.f+=1 end
	if(dead_trap.f>1) then dead_trap.f=0 end

    if (game_over_timer > 30) then
        spr(dead_trap.spr1+dead_trap.f*58,(dead_trap.tx-1)*16,(dead_trap.ty-1)*16,2,2)
    else
        spr(dead_trap.spr2,(dead_trap.tx-1)*16,(dead_trap.ty-1)*16,2,2)
    end
end

function ecollide_with_trap(t)
    btraps[#btraps+1] = {tx=t.tx, ty=t.ty, dt=destroy_time, sprt=sprite_nums.dtrap}
    del(traps,t) 
end

function pcollide_with_trap(t)
    if not p.hit_trap then
        p.hit_trap=true
        if not dead_sound_played then
            dead_sound_played=true
            sfx(3)
            game_over=true
            dead_trap.tx=t.tx
            dead_trap.ty=t.ty
            del(traps,t)
        end
        return
    end
end


-- end include/trap.lua
-- begin include/log.lua
function log(message)
    printh(message, "log.txt")
end
-- end include/log.lua
-- begin include/carpet.lua
function make_carpet()
    carpet={
        tx=get_tx(sprite_nums.carpet),
        ty=get_ty(sprite_nums.carpet),
        sprt=sprite_nums.carpet,
        w=4,
        h=2
    }
end
-- end include/carpet.lua
-- begin include/object.lua
function ecollide_with_objects(object_set, what_to_do)
    local etx = e.tx
    local ety = e.ty
	for o in all(object_set) do
		if (o.tx>=etx and o.tx<=etx+1) and (o.ty>=ety and o.ty<=ety+1) then
			what_to_do(o)
			return
		end
	end
end

function pcollide_with_objects(object_set, what_to_do)
    local ptx = p.tx
    local pty = p.ty
	for o in all(object_set) do
		if o.tx==ptx and o.ty==pty then
			what_to_do(o)
			return
		end
	end
end

function ecollide_with_object(object, what_to_do)
    local etx = e.tx
    local ety = e.ty
    if (object.tx>=etx and object.tx<=etx+1) and (object.ty>=ety and object.ty<=ety+1) then
        what_to_do(object)
    end
end

function pcollide_with_object(object, what_to_do)
    local ptx = p.tx
    local pty = p.ty
    if object.tx==ptx and object.ty==pty then
        what_to_do(object)
    end
end

function draw_objects(object_set)
    for o in all(object_set) do
        draw_object(o)
    end
end

function draw_object(object)
    local _w = object.w or 2
    local _h = object.h or 2
    spr(object.sprt,(object.tx-1)*16,(object.ty-1)*16,_w,_h)
end

function is_on_objects(tx, ty, object_set)
    for o in all(object_set) do
        if is_on_object(tx, ty, o) then return true end
    end
    return false
end

function is_on_object(tx, ty, object)
    return object.tx==tx and object.ty==ty
end
-- end include/object.lua
-- begin include/helps.lua
-- helps for n_lvls

help_texts={
    "elephants are afraid of mice. \nchase her through the door \nto help you escape.",
    "the elephant is hungry, she \nwill go for her food. but only \nif she feels safe! take cover!",
    "\n  pick up the peanut with ❎",
    "feed and hide! \npress 's' to toggle the \nelephant's line of sight",
    "help her get them peanuts! \npress 🅾️  or 'c' to retry.",
    "press the button to let the \nelephant out of her cage.",
    "the elephant will destroy some \nblocks when frightened. \nbut you can hide behind them.",
    "",
    "",
    "", --10
    "elephants drink 75 liters of \nwater daily. she can handle \nthis mess for you.", 
    "",
    "",
    "the mouse traps will catch you, \nbut they are no match for the \nelephant... ",
    "however she can see you \nover the traps.",
    "",
    "",
    "",
    "",
    "", --20
    "",
    "",
    "",
    "",
    "",
    "",
	"",
    "",
    "welcome to the warehouse! \nthanks for playing with us!", --29
}
-- end include/helps.lua
-- begin include/cheese.lua

function make_cheeses()
    cheeses=get_all_tile_pos(sprite_nums.cheese)
    eaten_cheeses={}
    semi_finish=false
    semi_finish_timer=100
end

function pcollide_with_cheese(c)
    p.cheese_eat_time-=1
    spawnpukk((p.tx-1)*16+8,(p.ty-1)*16+8,0,0,e.nut_eat_c.col1,e.nut_eat_c.col2,part)
    if p.cheese_eat_time <0 then
        eaten_cheeses[#eaten_cheeses+1] = {tx=c.tx, ty=c.ty, sprt=sprite_nums.dpeanut} -- add to deleted waters
        del(cheeses,c)
        sfx(8)
        p.cheese_eat_time=50
        if current_lvl==n_lvls and #cheeses==0 then
            semi_finish=true
        end
    end
end
-- end include/cheese.lua
__gfx__
00000000000000000000000000000000000b331300b31300000000000000000000000000066000000000000000000000000000000000000000000000779f99f9
0000000000000000000000000000000000000b33b33333300000000000000000000000066000000000000000000000000000000000000000000000779f99f9ff
00000000000000000000000000000000000b000331310b00000000000000000004999455444444400000000000000000000000000000000000000799999fffa9
000000055555000600000000000000000333330043b0000000000000000000004444444444444444000000333333333333333333330000000000799f9ff99aa9
000055555555500500000005555500000b01b33343000000000000000000000094455555555554440000333bbbbbbb333bbbbbbbb3330000000799fffa9749a9
00557755555550050000555555555000000000034b13b00000000000000000009455777755555544000333b333333333b3333333333330000079ff99aa97a9a9
0555c755555550550055775555555006000b033333333000000000000000000094575555555555440003333333333333333333333333300000ff97449aa99aa9
e5555555555555500555c75555555055000333040b0b33000000000000000000945755555555554400011133333333333333333333111000009a97aa9aaaaaa9
0555c75555555000e5555555555555500b33b0b33000033b000000000000000094575555555555440013bb133333333333333333313bb100009a9aaa9aaaaaa9
00557755555550000555c7555555500003312224332e013100000000000000009457555555555544001b3313333333333333333331b331000099a999a999aaa9
0000555555555000005577555555500033002722b32e0b3000000000000b00004457555555555544001b3313111111131111111131b3310000749aaa9749a999
0000000555550000000055555555500000002722233e000000000000003300004457555555555544001b3311333333313333333311b33100007a9aaa97a99900
0000000000000000000000055555000000002722222e000000000000b33100004455555555555544001333133333333333333333313331000099aaaa99990000
000000000000000000000000000000000000027222e00000003333b233333300444555555555544400013333333333333333333333331000009aaaa999000000
000000000000000000000000000000000000027222e00000033b133332ee1330444444444444444400013333333333333333333333331000009aa99900000000
000000000000000000000000000000000000022222e00000331772233222eb334444555555854444000044440000000000000000444400000099900000000000
0000000e0000000000000000e000000000677777777777600000007777700000000000000000000000000000000000000000000000000000000007e88e900000
000000555000000000000005550000000067777666777760000000000000000000000000000000000000000000000000000000000000000000007ee88ee90000
000005555500000000000055555000000067777777677760000070000000760000000000000000000000000000000000000000000000000000007ee88ee90000
00000555550000000000005555500000006777777777776000007777777776001111111111111111000000088888880008888888800000000007ee8888ee9000
000057c5c75000000000057c5c7500000066666666666660000066666666660015556565555555510000008eeeeeee808eeeeeeee80000000007ee8888ee9000
00005775775000000000057757750000000006777766000000770677766cc1001556565555555551000088e888888888e888888888880000007ee888888ee900
000055555550000000000555555500000000067777760000007706777760cc00156565555555555155888888888888888888888888888855007ee888888ee900
000555555555000000005555555550000000677ddd7760000077000dd7767c10165655555555555188588888888888888888888888888588000ee888888ee000
00055555555500000000555555555000000067ddcdd760000777c0c7cd7677701565555555555551588588888888888888888888888858850000088888800000
00055555555500000000555555555000000067dcccd76000777cc7cc777677701655555555555551058858888888888888888888888588500000007666000000
00055555555500000000555555555000000067dcc1d76000777c67d00cc1177715555555555555510588588eeeeeeee88eeeeeeee88588500004444444444000
00055555555500000000555555555000000067d7ccd7600000cc67d700cc1c77155555555555555100588ee88888888ee88888888ee885000004ffffffff4000
00005555555000000000055555550000000067dcccd760000ccc67dddd76cc771111111111111111005888888888888888888888888885000004f44444554000
000000050000000000000000500000000000677ddd7760000011677dd776cccc0000000110000000000588888888888888888888888850000004f44444444000
00000005500000000000000055000000000007777776000000000777776cc1110000077111100000000588888888888888888888888850000004f44444444000
00000000555600000000000005600000000000777660000000000077660000000000711111110000000044440000000000000000444400000000440000440000
00005555555555555555555555550000000000000000000000000000000000000000555555555555555555555555000000000000000000000000000000000000
00555666ee224499994422ee666655000000555555555555555555555555000000555666ee224499994422ee6666550000005555555555555555555555550000
055666666e222449944222e66666655000555666ee224499994422ee66665500055666666e222449944222e66666655000555666ee224499994422ee66665500
056666666ee2224444222ee666666650055666666e222449944222e666666550056666666ee2224225555ee666666650055666666e222449944222e666666550
5566666666ee222225555e6666666665056666666ee2224444222ee6666666505566666666ee22225566656666666665056666666ee2224225555ee666666650
56666666666eee2255666566666666655566666666ee222225555e666666666556666666666eee2556666666665566655566666666ee22225566656666666665
5666666666666ee5566666666666666556666666666eee2255666566666666655665656666666ee5666666666577666556666666666eee255666666666556665
566666666666666566666666555566655666666666666ee55666666666666665565666566666666566666666577766655666666666666ee56666666665776665
56656666666666656666666577776665566666666666666566666666555566655666566666666665666666657711666556666666666666656666666657776665
55665666666666656666666577776665566656666666666566666665777766655566566666666665666666657711666556656666666666656666666577116665
55665666666666656666666577116665556656666666666566666665777766655566566666666665666666657777666555665666666666656666666577116665
56556666666666656666666577116665556656666666666566666665771166655655666666666665666666665777666555665666666666656666666577776665
56666666666666656666666666666655565566666666666566666665771166655666666666666665566666666666665556556666666666656666666657776665
56666666666666655666666666666655566666666666666566666666666666555666666666666666556666566666665556666666666666655666666666666655
56666666666666665566665666666665566666666666666556666666666666555666666666666666656665566666666556666666666666665566665666666655
56666666666666666566655666666665566666666666666655666656666666655666666666666666655656666666666556666666666666666566655666666665
56666666666666666556566655666655566666666666666665666556666666655666666666666666665556665566665556666666666666666556566666666665
56666666666666666655566577566655566666666666666665565666556666555666666666666666666666657756665556666666666666666655566655666655
56666666666666666666666577566665566666666666666666555665775666555666666666666666666666657756666556666666666666666666666577566655
56666666666666666666666577556665566666666666666666666665775666655666666666666666666666657756666556666666666666666666666577566665
56666666666666666666666577756655566666666666666666666665775666655666666666666666666666657775665556666666666666666666666577566665
56666666666666666666665557755655566666666666666666666665777566555666666666666666666666555775565556666666666666666666666577756655
56666566666566666566665005775665566666666666666666666655577556555666656666656666656666500577566556666666666666666666665557755655
56666566666566666566665000575665566665666665666665666650057756655666656666656666656666500057566556666566666566666566665005775665
56666566666566666566665055555655566665666665666665666650005756655666656666656666656666500005565556666566666566666566665000575665
56666566666566666566665056505665566665666665666665666650555556555666656666656666656666500000565556666566666566666566665000055655
56666566666566666566665056505665566665666665666665666650565056655666656666656666656666500000566556666566666566666566665000005655
56666566666566666566665056556665566665666665666665666650565056655666656666656666656666500000566556666566666566666566665000005665
56666566666566666566665056666650566665666665666665666650565566655665556665556665556655500000565556666566666566666566665000005665
56666566666566666566665056666500566665666665666665666650566666500555505555505555505555000000565556666566666566666566665000005655
56655566655566655566555005555000566555666555666555665550566665000000000000000000000000000000555556655566655566655566555000005655
05555055555055555055550000000000055550555550555550555500055550000000000000000000000000000000000005555055555055555055550000005555
4444444444444444000000000000000000ddcccddc00000000000000000000000000000000000000000000222200000000000000000000000000000000000000
499a4999999949940000000000000000007ccdcccccc000000000000000900000000000000000000000000222200000000000000000000000000000000000004
4999499999a949940000000000000000cccccdddddccc00000000011111190000000000000000000111116222211116600000000044400000000000000000044
4999499999994aa40000000000000000cddcccccccdccc0001111100119119900000008888800000100000222e000001000000044af940000000000000000444
44444444444444440000000000000000ccdc11cccdccccdc0011000dd9991199000008eeef780000111161222e1111610000004ff9fa94000000000000000555
49999999499999940000000000000000dcccdcccd777ccdd00001dd99991111900008eeeeef7800010000022e200000100000049ffff94000000000000000666
49aa9999499999940000000000000000dcccddddccccddcc000011191111d9aa000088eeeef8800011166122e21116110000049ff9fa94000000000000000444
499999994999a9940000000000000000ccdddccccc111ccc000dd911199daa4400008e888887800010000022e2000001000449ffa9f994000000000000000444
44444444444444440000000000000000ccd77777cccc1ccc0999999999da444000048eeeeef7840011161122e2111611004fffff999940000000000000000444
49994999999949940000000000000000111ccccddddccddc999999999da4400000448eeeeef784401000002e2200000104af9f9a994400000000000000000444
4aa94a99999949940000994400094000ccdddcccddcccdcc4a999999da440000004448eeeff844401661112e2211611104f9fff9940000000000000000000444
49994999999a49a40449904999049400cccccccccccccc7c04a999dd4440000000044444444444001000002e2200000104999a99400000000000000000000555
444444444444444409049009949909a00ddddddcc777ccc00044a9a4440000000000444444444000161111e222116111049a9999400000000000000000000666
4999a99949999994009a9444994994400cccdcccccdddd0000044a44000000000000000000000000100000e22200000100499994000000000000000000000444
49999999499a9994000004994044a90000c1777cd77ccc0000004440000000000000000000000000661111222216111100044440000000000000000000000a44
44444444444444440000000000000000000cccccccc0000000000000000000000000000000000000000000222200000000000000000000000000000000000aa4
4444445444445444444445000054444400cddcccdd00000005556000000000000000000000000000002200000000220044444444444444440000000000000aa4
55594959aa995994499a4500005949940077ccdccccc0000500000000119000000000000000000000022000000002200499a4999999949940000000000000a44
49594559999955544999457600594994ccccccdddddcc000500000511551900000000000000000001122000000002e164999499999a949940000000000000444
4a994599999949554999455765594aa4ccddcccccccdcc0005555515555119900000000000000000102200000000ee014999499999994aa40000000000000666
44445554444444444444445555444444cccdc11cccdccccd00055155555511990000003333300000112200000000e26144444444444444440000000000000555
49995955559999944999999949999994ddcccdcccd777ccd0055155555555119000003bbbf730000102200000000e20155559999499955550000000000000444
4aa559994999995549aa999949999994cdcccddddccccddc005155555555511a00003bbbbbf73000112200000000e26100055999499550000000000000000444
555599994999a554499999994999a994cccdddccccc111cc0015555555551544000033bbbbf33000102e00000000220160005999499560000000000000000444
44444444444444444444444444444444cccd77777cccc1cc091155555551554000043b3333373400162e00000000221176005444444576000000000000000444
499949aa999949944999499999994994c111ccccddddccdd999115555515775000443bbbbbf73440102e00000000220176055999999557000000000000000444
45594959999559944aa94a9999994994cccdddcccddcccdc4a99115551555c50004443bbbff34440162e00000000221155554a99999955550000000000000666
55a949555555455549994955559a49a4ccccccccccccccc704a9911515755550000444444444440010e200000000220149994999999a49a40000000000000555
444444444544444444444550055444440cddddddcc777cc00044a911557c5550000044444444400061e200000000221144444444444444440000000000000444
49999a99559999944999a500005999940ccccdcccccdddd00004444405555550000000000000000010220000000022014999a999499999940000000000000044
4999955559999aa449999566005a999400cc1777cd77cc0000004440000555e00000000000000000612200000000221149999999499a99940000000000000004
44445544444444444444457760544444000cccccccc0000000000000000000000000000000000000002200000000220044444444444444440000000000000000
00000000000000000000000000000000000005555550000000000000000000000000000004000000000000000000000066666666666666666666665000566666
0000000000000000000000000000000000055d6111d550000000000000000000000000a00090000000000000000000006777777d6777777d677777500057777d
0000000000000000000000000000000000511d6111d615000000000004440000099040000400000000000000011000006777777d6777777d677777560057777d
0000000000000000000000000000000005611d6111d61150000000044af94000000a0090a000900000011110001000006777777d6777777d677777576657777d
0000000000000000000000000000000005666d6666d666500000004ff9fa940000009000000000000017cc10000000006777777d6777777d677777755577777d
000000000000000000000000000000005dddddddddddddd500000049ffff94000000000000040a0000001cc000000000dddddddd6ddddddddddddddd6ddddddd
0000000000000000000000000000000056111d6111d611150000049ff9fa94000900404009000040000000000011110066666666666666666666666666666666
0000000000000000000000000000000056111d6111d61115000449ffa9f994000000000090000000000000000cccc1006777777d6777777d6777777d6777777d
0000000000000000000000000000000056111d6111d61115004fffff9999400000000a040090900000000000071c00006777777d6777777d6777777d6777777d
0000000000000000000000000000000056666d6666d6666504af9f9a99440000000400040000040000000000011000006777777d6777777d6777777d6777777d
000000000000000000000000000000005dddddddddddddd504f9fff99400000000004090009000000000000000000000dddddddd6ddddddddddddddd6ddddddd
0000000000000000000000000000000005611d6111d6115004999a9940000000409a000040004000000c11110001100066666666666666666666666555666666
0000000000000000000000000000000005611d6111d61150049a99994000000000040a9000400a000017c110000111006777777d6777777d677777566757777d
00045644444564aaaa4654444465400000566d6111d6650000499994000000000040000090a090a000110000000000006777777d6777777d677777500657777d
004456444445644aa44654444465440000055d6666d550000004444000000000000090090940040000000000000000006777777d6777777d677777500057777d
044456444445644444465444446544400000055555500000000000000000000000000000000000000000000000000000dddddddd6ddddddddddddd50005ddddd
00000000000000000000000000000000999999999999999999900999940099000000000000000000000000000000000000000000000000006666666666666666
00000000011900000000000009090000944454454454544194400445440045000000000001190000992288228822288228822288228822006777777d6777777d
55500051155190000000000090019000954454444454454194440444400445000000005115519000008822992288822992288822992288896777777d6777777d
60555515555119900000000090011990954111111111954194400044400415000555551555511990082299ff9922299ff9922299ff9922006777777d6777777d
000551555555119900000009000991999441449445449541044044400004954050055155555511990029f9229f929f9229f929f9229f92826777777d6777777d
00551555555551190000099900000019945155494544954100004400004494405055155555555119829f222222f9f222222f9f222222f900dddddddd6ddddddd
005155555555511a000000000099901194511444944594514000000440449450505155555555511a00992288229f92288229f922882299205555666666665555
0015555555551544000dd9999009910194514145594594519454004440049450601555555555154429f22822822922822822922822822f000006577d67757600
0911555555515540090099999909441095515414449594519450000444004440091155555551554000f22822822922822822922822822f920006577d67756000
9991155555157750999000000009400095415441544994519450050044000004999115555515c75002992288229f92288229f922882299000067577d67756000
4a99115551555c509999001110940000954154541555945100504544400440004a99115551555550009f222222f9f222222f9f222222f9285555dddd6ddd5555
04a9911515755550000000dd109000009541545441449451000045400044940004a9911515c555502829f9229f929f9229f929f9229f92006666666666666666
0044a911557c55501111900410900000954199999999944194440000049994550044a91155755550002299ff9922299ff9922299ff9922826777777d6777777d
00044444055555500004490000000000954454445454545194400004445444400004444405555550988822992288822992288822992288006777777d6777777d
00004440000555e000004440000000009454544454545451940044444454544000004440000555e0002288228822288228822288228822996777777d6777777d
0000000000000000000000000000000011111111111111110000400000000000000000000000000000000000000000000000000000000000dddddddd6ddddddd
__label__
11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
11ddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffd11
11dfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11dfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffff11
11ddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffd11
11fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fddddddddddd11
11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
11111111111111111000000000001000001111111000000000001000000000001000001000001000000000001000000001111000000000001111111111111111
11111111111111111000000000001000001111111000000000001000000000001000001000001000000000001000000001111000000000001111111111111111
11111111111111111009999999991009991111111009999999991009999999991009991009991009999999991009999991111009999999991111111111111111
11111111111111111009999999991009991111111009999999991009999999991009991009991009999999991009999990001009999999991111111111111111
11111111111111111009999999991009991111111009999999991009999999991009991009991009999999991009999990001009999999991111111111111111
11111111111111111009991111111009991111111009991111111009991009991009991009991009991009991009991009991111009991111111111111111111
11111111111111111009990001111009991111111009990001111009990009991009990009991009990009991009991009991111009991111111111111111111
11111111111111111009990001111009991111111009990001111009990009991009990009991009990009991009991009991111009991111111111111111111
11111111111111111009999991111009991111111009999991111009999999991009999999991009999999991009991009991111009991111111111111111111
11111111111111111009999991111009991111111009999991111009999999991009999999991009999999991009991009991111009991111111111111111111
11111111111111111009999991111009991111111009999991111009999999991009999999991009999999991009991009991111009991111111111111111111
11111111111111111009991111111009991111111009991111111009991111111009991009991009991009991009991009991111009991111111111111111111
11111111111111111009990000001009990000001009990000001009991111111009991009991009991009991009991009991111009991111111111111111111
11111111111111111009990000001009990000001009990000001009991111111009991009991009991009991009991009991111009991111111111111111111
11111111111111111009999999991009999999991009999999991009991111111009991009991009991009991009991009991111009991111111111111111111
11111111111111111009999999991009999999991009999999991009991111111009991009991009991009991009991009991111009991111111111111111111
11111111111111111009999999991009999999991009999999991009991111111009991009991009991009991009991009991111009991111111111111111111
11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
11111111111111111111119999991199991111111111119999991199119911999999111111111199999911119999111199991199999911111111111111111111
11111111111111111111119999991199991111111111119999991199119911999999111111111199999911119999111199991199999911111111111111111111
11111111111111111111111199111199119911111111111199111199119911991111111111111199119911991199119911991199999911111111111111111111
11111111111111111111111199111199119911111111111199111199119911991111111111111199119911991199119911991199999911111111111111111111
11111111111111111111111199111199119911111111111199111199999911999911111111111199991111991199119911991199119911111111111111111111
11111111111111111111111199111199119911111111111199111199999911999911111111111199991111991199119911991199119911111111111111111111
11111111111111111111111199111199119911111111111199111199119911991111111111111199119911991199119911991199119911111111111111111111
11111111111111111111111199111199119911111111111199111199119911991111111111111199119911991199119911991199119911111111111111111111
11111111111111111111119999991199119911111111111199111199119911999999111111111199119911999911119999111199119911111111111111111111
11111111111111111111119999991199119911111111111199111199119911999999111111111199119911999911119999111199119911111111111111111111
11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
11dfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000077707770555005500550000000000000000000000000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000070707070777057705000000005555500000000000000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000077707700750075505770000055050550000000000000000000000000000077707770000000000000000000000000000011
11000000000000000000000000000070007070770077707050000057777750000077700550000007707770777075705750000000000000000000000000000011
11000000000000000000000000000070007070755055707770000077070770000007005770000070000700707077500700000000000000000000000000000011
11000000000000000000000000000000000000777077000070000077757770000007007070000077700700777075700700000000000000000000000000000011
11000000000000000000000000000000000000000000007700000077070770000007007070000000700700707070700700000000000000000000000000000011
11000000000000000000000000000000000000000000000000000007777700000007007570000077000700707050500500000000000000000000000000000011
11000000000000000000000000000000000000000000000000000000000000000000007700000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11dfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffff11
11ddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffd11
11fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fddddddddddd11
11000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000000000000000004440000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000000000000000049994000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000000000000004999999400000000000000000000000000000000000000000000000000000011
1100000000000000000000000000000000000000000000000000000000000004999a9a9400000000000004440000000000000000044400000000000000000411
110000000000000000000000000000000000000000000000000000000000044999fff9940000000000044af940000000000000044af940000000000000044a11
11000000000000000000000000000000000000000000000000000000000449999a9f99f400000000004ff9fa940000000000004ff9fa940000000000004ff911
1100000000000000000000000000000000000000000000000000000000499f9a9fffffa4000000000049ffff9400000000000049ffff9400000000000049ff11
110000000000000000000000000000000000000000000000000000000049af9fff94440000000000049ff9fa940000000000049ff9fa940000000000049ff911
110000000000000000000000000000000000000000000000000000000049fffff94000000000000449ffa9f994000000000449ffa9f994000000000449ffa911
110000000000000000000000000000000000000000000000000000000004af9f940000000000004fffff999940000000004fffff999940000000004fffff9911
110000000000000000000000000000000000000000000000000000000004ffa4f4000000000004af9f9a99440000000004af9f9a99440000000004af9f9a9911
110000000000000000000000000000000000000000000000000000000004440000000000000004f9fff994000000000004f9fff994000000000004f9fff99411
110000000000000000000000000000000000000000000000000000000000000000000000000004999a9940000000000004999a9940000000000004999a994011
1100000000000000000000000000000000000000000000000000000000000000000000000000049a9999400000000000049a9999400000000000049a99994011
11000000000000000000000000000000000000000000000000000000000000000000000000000049999400000000000000499994000000000000004999940011
11000000000000000000000000000000000000000000000000000000000000000000000000000004444000000000000000044440000000000000000444400011
11000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000009990099009909990990000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000009090909090909990090000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000009900909090909090090000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000009090909090909090090000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000009090990099009090999000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011
11000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011
11fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fddddddddddd11
11ffffffffffffff6fffffffffffffff6fffffffffffffff6fffffffffffffff6fffffffffffffff6fffffffffffffff6fffffffffffffff6fffffffffffff11
11fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fdddddddddddf1f1fddddddddddd11
11ddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffddfffddfffffffffd11
11dfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11fffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdfffffffffffffdfdffffffffffff11
11dfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffffddfddfffffffffff11
11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111

__map__
800a0c0400008080200000800e0e0e802000000000008080cccccccccccccccc0000000000000000000a0c00000000008080808004242e0000ac00ac8cac00040080e48c8a008c8f0a0c082a2c2e0480000000ee00000000880000cccc000000040088c40000c48c040a0c008686008f2a2c8000000000008f8f008800000000
2eeaec000000008080400080808080808080808080008080cc002400002400cc000000000000000000002000000000008020008000000000a24000008000000000808ce48a00008f0000000000000080000000cc00000000cc400000840000008080a28080808a8a000000008686008f0000ac00008c20008a8a868680808a8a
000000000000008080000080042a2c80e4e42800008c8080cc000000008c00cc000080808080000000eaec0000000000800000ac0000000000000000000000a200ac000000808a8aa280808080a280800024cccccc240000cc0000008400208c00c4400000e40000808080808c0000000000808080800080008a004000a0008c
000000088000008080000080eaec008008e4280080008080cccccccc00cccccc000080e4e4e4000004282e8000000000800000808080a280a28000000000000000808a8a8a808c004000c400c400008f008440000084008fcc8a8acccc00000000c4000000e4000000400000e4e48000868640000086008f008a000000a00000
800000808000008f800000000000008fe4e428400000008f400000000000008f0000808c8c8c00004000008a00008c8f8000004000e4008f00000000000000a20080000000808c000000c400c400008f008400000084008f00000000840000840080000000808a8a00000000e4e48000868600000086008f8a8a800000ac8a8a
804000000000008f80000000008c008fe408e4000000008f000000200000008f40008020e4e400000000008a0000008f8000000000e4008fa20000000000000000808c8c8c808c0000800000cccccccc0084cccccc84ceccc4c4c400840000840080000000800000a28080800000000000008080808000800000a00000808686
8000008080cccccc808080808080808028e4e42828e42880cccccccccccccccc000080e4e4e400008080808000808080808080808080808000000000800000a200808080808040008c0020ee00002400008400000084208c00848400ee0000000000208c00ac00000000000000eaec000028800a0c0000000000a08c00000000
8000000020cc24cc8080808080808080e4282808e4e40880ccccc4ccccc4cccc8080808080808f8f0e0e0ea00088808080808080808080800420ac00ac8f8f2e2000000088800000808080cc000000000084000000840000008f8fcccccccccc0000000000808f8f002000000000000000048000000000008c00ac2000000000
8886000000008a8c8c000000000086888f8a0088008600008f00a000000000c48f000088000000008c002880000000888800000000000000800e08800a0c8000808f8f80008cac0004000000ee8c882400008a00008a00000000008f8f8a00002880808080808028000000000000000000000000000000000000000000000000
860000000000008a00000000000000868f8ae4c4c4e400008f00a000000000c48f00040000a08a8a00000080000004a200e4e40000000000808080800000800000e4e42000c400c400000084c4e4868f8400000000008a8f8a8a880020000000808c0e8c0e008c80000000000000000000000000000000000000000000000000
000080000080000000008000008000000000864000e40000c4c480a2800000000000864000a000008080a280008080008c00000000000086808880808a8a80000000000080c400000000cc84c4e4868f888400000000008f000080a0a0008c00808c0040000e0080000000000000000000000000000000000000000000000000
000000400000000000000040000000008a8a860000e400008c200000008600008a8a860000a000004000000086008600e4400000e4008a8f4000008a86c400000000000080c400868a8acc0000244000800000000000008a000080808a00008080000e00008c0e80000000000000000000000000000000000000000000000000
00000000000000000000000000000000000080000080000000008080800000000000800000808a8a0000000086008a00e400000000e48a8f0000008a00e4800000c4000080c486880000cc00c40000000000e44000000086000080808a00008080000e8c00008c80000000000000000000000000000000000000000000000000
0000800000808c8a000080000080008a0000ac0000ac86864000c40000008c000000ac0000ac86868080000000ac000020000000000000860000008000eaec00808c86008ac400860024cc8a8acccc8600e400000000a08c868600808000000080008c00000e0080000000000000000000000000000000000000000000000000
0000000000008a8f0000000000008a8f0000e400008c00000000c400000000a20000a000008c00000000000000808a8a0000000000000000868080800000008f808000008a86400000000000008c20008a0000000000a080008a8a4000000000808c0e2000008c80000000000000000000000000000000000000000000000000
2000000000008a8f2000000000008a8f0000e4000020000080808000000000008c00a000002000000020000000808f8f00acac000000000000acac20008c008f00000000ac860000e40400000000e4e4000020acac00008c00008a000000008a0880808080808008000000000000000000000000000000000000000000000000
__sfx__
01030000070650a0650f0650500204003050030000000000090650b06500065000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
cf080000136600c660136600b660136600b660136600c660136600c660136600b660136600b660136600c660136000b6000060000600006000060000600006000060000600006000060000600006000060000600
011000001c1651f16523165281651d1052616528160281622816228165001052a1052a1052a1052a1052a1052b1052b1050010500105001050010500105001050010500105001050010500105001050010500000
7b10000021165201651f1651e1651d1621d1621d16500100001000010000100001000010000100001000010000100001000010000100001000010000100001000010000100001000010000100001000010000100
010400000550204503045640456404564005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500000000000000000
430c00001f16526165001051a6051a6051a6051a60500105001050010500105001050010500105001050010500105001050010500105001050010500105001050010500105001050010500105001050010500005
010400001806019060190600000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
07030000107600e760177601c7621c765007000070000700007000070000700007000070000700007000070000700007000070000700007000070000700007000070000700007000070000700007000070000700
d70600001056213562185621356210562135621856213562105621356218562135651050000500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500
011000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
cb1200202904529035290252901529015290153004530035300253001530015300152d0452d0352d0252d0152b0452b0352b0252b0252b0252b0153204532035320253202532015320152b0452b0352b0252b015
cb1200202904529035290252901529015290153004530035300253001530015300152d0452d03529025290152b0452b0352b0252b0252b0252b0153204532035320253201530045300252d0452d0252b0452b025
cb1200202904529035290252901529015290153004530035300253001530015300152d0452d03529025290152b0452b0352b0252b02528045280352904529025180451802530045300252d0452d0252b0452b025
631200200003000030000420005200052000520905009050090520905209052090520505205052090520905207050070500705207052070520705207052070530700007000070000700007000070000700007000
011200200c0430000000000000000c635000000c043000000c0430000000000000000c6350000000000000000c6000000000000000000c600000000c0000c6000c6000c6000c0000c6000c600000000c6000c000
d31200201d5201d5201d5201d5201d5221d522245202452024520245202452224522215202152021522215221f5201f5201f5201f5201f5221f5222652026520265202652026522265221f5201f5201f5221f522
cb1200202904529035290252901529015290153004530035300253001530015300152d0452d03529025290152b0452b0352b0252b0252b0252b0153204532035320253201530045300252d0452d0252b0452b025
011200200c04300000000000000000000000000c04300000000000000000000000000c6350000000000000000c04300000000000000000000000000c0430c6000c6000c6000c0430c6000c635000000c04300000
cb1200202904529035290252901529015290153004530035300253001530015300152d0452d0352d0252d0152b0452b0352b0252b0252b0252b0153204532035320253202532015320152b0452b0352b0252b015
63120020000300003000032000420005200052070500705007052070520705207052040520405204052040520505005050050520505205052050520a0500a0500a0520a0520a0420a03207052070420703207025
631200200003000030000420005200052000520905009050090520905209052090520505205052090520905207050070500705207052070520705204050040500405204052040520405202052020420203202035
d31200201d5201d5201d5201d5201d5221d522245202452024520245202452224522215202152021522215221f5201f5201f5201f5201f5221f5222652026520265202652026522265221f5201f5201f5221f522
d31200201d5201d5201d5201d5201d5221d52224520245202452024520245222452221522215221d5221d5221f5201f5201f5201f5201f5221f52226520265202652226522245222452221522215221f5221f522
011200200c0530000000000000000c635000000c05300000000000000000000000000c6350000000000000000c0530000000000000000c635000000c0530c6000c6000c6000c0530c6000c635000000c05300000
cb1200202904529035290252901529015290153004530035300253001530015300152d0452d03529025290152b0452b0352b0252b02528045280352904529025180451802530045300252d0452d0252b0452b025
63120020000300003000032000420005200052050520505207050070500705207052040500405004052040520505005050050520505205052050520a0500a0500705007050070520705204050040500404204035
631200200003000030000320004200052000520505205052070500705007052070520405004050040520405205050050500505205052070500705007052070520405004050040520405202050020500204202035
011200200c0430000000000000000c635000000c043000000c0430000000000000000c6350000000000000000c6350000000000000000c635000000c0430c6350c6350c6000c0430c6000c635000000c6350c000
011200200c0430000000000000000c635000000c043000000c0430000000000000000c6350000000000000000c6000000000000000000c600000000c0000c6000c6000c6000c0000c6000c600000000c6000c000
631200200003000030000420005200052000520905009050090520905209052090520505205052090520905207050070500705207052070520705207052070530700007000070000700007000070000700007000
d31200201d5201d5201d5201d5201d5221d52224520245202452024520245222452221522215221d5221d5221f5201f5201f5201f5201c5221c5221c5201c5201d5221d5221d5221d5221d5221d5221d5221d515
__music__
01 13514344
00 1a514344
00 19514344
00 14514344
00 13114344
00 1a114344
00 19114344
00 141b4344
00 13171244
00 1a171044
00 19171244
00 14171044
00 13171244
00 1a171844
00 19171244
00 1d1c1844
00 13111215
00 1a111016
00 19571215
00 145c1816
00 13515215
00 1a515816
00 19175215
00 141b5816
00 13111215
00 1a111016
00 19171215
00 141c1816
00 13115215
00 1a115016
00 19575215
02 1d5b581e

