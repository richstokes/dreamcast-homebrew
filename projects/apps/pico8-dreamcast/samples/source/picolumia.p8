pico-8 cartridge // http://www.pico-8.com
version 29
__lua__
-- picolumia
-- by andrew edstrom

local board
local player
local next_quad
local particles
local speed_timer
local speed
local last_direction_moved -- "right" or "left"
local game_state -- "playing", "gameover", "menu", "won"
local hard_dropping
local number_of_sounds = 10
local combo_size

-- when you first hold down a button, how many frames before it repeats
-- defaults to 15 on pico8 but we override it a la https://twitter.com/lexaloffle/status/1176688167719587841?s=20
local btnp_inital_delay = 6
poke(0x5f5c, btnp_inital_delay)

-- player config
local display_shadow
local swap_rotation_buttons
local display_milliseconds

-- coroutines
local drawing_combo_text
local blocks_clearing

-- values to display in hud
local cleared
local score
local level
local seconds_elapsed
local seconds_timer

-- block types
local white_block = 8
local red_block = 16
local yellow_block = 24
local blue_block = 32
local empty = 40
local wall = -1

-- board constants
local board_outline_color = 13
local board_left = 8 -- Used to center the board
local board_height = 27 -- must be odd for math to work out
local board_width = 8
local bottom = 117
local piece_width = 8
local piece_height = 4
local sprite_size = 6

-- game feel thiccness (a.k.a. juice)
local x_shift
local y_shift
local shimmy_coefficient=1.4
local shimmy_degredation_rate=.93
local minimum_shimmy_threshold=.8
local fade_speed=0.05
local current_fade_perc=0
local loading=false

-- set up palette

-- begin utils.lua
-->8
-- random utils

function setup_palette()
    pal()
    _pal={0,129,136,140,1,5,6,7,8,135,10,131,12,13,133,134}
    for i,c in pairs(_pal) do
        pal(i-1,c,1)
    end
end

function currently_clearing_blocks()
    return blocks_clearing and costatus(blocks_clearing) ~= 'dead'
end

function for_all_tiles(callback)
    for y = 1, board_height do
        for x = 1, board_width do
            if board[y][x] ~= wall then
                callback(y, x)
            end
        end
    end
end

function is_odd(num)
    return num % 2 ~= 0
end

function get_screen_position_for_block(y,x)
    local x_loc=x*piece_width + board_left
    if is_odd(y) then
        x_loc += piece_width/2
    end
    local y_loc=bottom-y*piece_height
    return y_loc, x_loc
end
-- end utils.lua
-- begin config.lua
-->8
-- configuration options

-- shadow
function turn_on_shadow()
    display_shadow = true
    menuitem(1, "hide shadow", turn_off_shadow)
end

function turn_off_shadow()
    display_shadow = false
    menuitem(1, "show shadow", turn_on_shadow)
end

-- inverse rotation buttons (for mobile players)
function standard_rotation_mode()
    swap_rotation_buttons = false
    menuitem(2, "inverse rotation", inverse_rotation_mode)
end

function inverse_rotation_mode()
    swap_rotation_buttons = true
    menuitem(2, "normal rotation", standard_rotation_mode)
end

-- millisecond display (for speedrunners)
function turn_on_millisecond_display()
    display_milliseconds = true
    menuitem(3, "hide millis", turn_off_millisecond_display)
end

function turn_off_millisecond_display()
    display_milliseconds = false
    menuitem(3, "show millis", turn_on_millisecond_display)
end
-- end config.lua
-- begin juice.lua
-->8
-- juice

function doshake()
    local x_pos = flr(x_shift)
    if x_shift < 0 then
        x_pos = ceil(x_shift)
    end
    camera(x_pos,y_shift)
    x_shift *= shimmy_degredation_rate
    y_shift *= shimmy_degredation_rate
    if abs(x_shift) < minimum_shimmy_threshold then
        x_shift = 0
    end
    if abs(y_shift) < minimum_shimmy_threshold then
        y_shift = 0
    end
end

-- whole_screen = 1 means swap palette for whole existing screen
-- whole_screen = 0 means just change the draw palette
function fadepal(_perc, whole_screen)
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
    local p = flr(mid(0, _perc, 1) * 100)

    local kmax, col, dpal, j, k

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
    dpal = {0, 1, 1, 2, 1, 13, 6, 4, 4, 9, 3, 13, 1, 13, 14}

    for j = 1, 11 do -- should go to 15 but we never need to fade colors above 11
        col = j

        -- now calculate how many
        -- times we want to fade the
        -- color.
        -- this is a messy formula
        -- and not exact science.
        -- but basically when kmax
        -- reaches 5 every color gets
        -- turned black.
        kmax = (p + (j * 1.46)) / 22

        -- now we send the color
        -- through our table kmax
        -- times to derive the final
        -- color
        for k = 1, kmax do col = dpal[col] end

        -- finally, we change the
        -- palette
        pal(j, col, whole_screen)
    end
end


function particles_for_block_clear(y, x, block_col)
    -- at first just spawn one
    local x_loc
    local y_loc
    y_loc, x_loc = get_screen_position_for_block(y,x)
    local i
    local number_of_particles = 10 + 2 * min(level, 10)

    for i = 1, number_of_particles do
        -- determine particle color
        local col = 7
        if i % 7 ~= 0 or block_col == white_block then
            -- most particles are white
            col = 7
        elseif block_col == blue_block then
            col = 12
        elseif block_col == red_block then
            col = 8
        elseif block_col == yellow_block then
            col = 10
        end

        -- create particle
        add(particles, {
            x=x_loc,
            y=y_loc,
            r=rnd(2),
            color=col,
            mult=rnd(1)/300,
            ttl=40+rnd(30),
            fade_perc=0,
            starting_theta=rnd(1),
            update=function(self)
                self.r = self.r + 1.8
                self.ttl = self.ttl - 1
                if self.ttl < 20 then
                    self.fade_perc = self.fade_perc + fade_speed
                end
            end,
            draw=function(self)
                theta = self.r * self.mult + self.starting_theta
                local spiral_x = self.r * cos(theta)
                local spiral_y = self.r * sin(theta)
                local x_coord = self.x + spiral_x
                local y_coord = self.y + spiral_y

                if x_coord > 128 or x_coord < 0 or y_coord > 128 or y_coord < 0 then
                    self.ttl = -1
                else
                    if self.fade_perc > 0 then
                        fadepal(self.fade_perc, 0)
                    end

                    pset(x_coord, y_coord, self.color)

                    if self.fade_perc > 0 then
                        setup_palette()
                    end
                end
            end,
            is_expired=function(self)
                return self.ttl < 0 or self.fade_perc > 1.1
            end
        })
    end
end
-- end juice.lua
-- begin clearing-blocks.lua
-->8
-- clearing blocks

function yield_n_times(n)
    local i
    for i=1,n do
        yield()
    end
end

function calculate_points_scored(blocks_cleared)
    return (level+1)*((blocks_cleared-2)^2)
end

function let_pieces_settle()
    --todo do this as a coroutine too
    local falling=true
    while falling do
        falling=false
        -- todo make this happen over multiple frames
        for_all_tiles(function(y, x)
            if board[y][x] ~= empty then
                if block_can_fall_left(y,x) then
                    move_piece(y, x, y-1, x_for_next_row(y,x))
                    falling=true
                end
            end
        end)
        if falling then
            yield_n_times(2)
        end
        local falling_right = false
        for_all_tiles(function(y, x)
            if board[y][x] ~= empty then
                if block_can_fall_right(y,x) then
                    move_piece(y, x, y-1, x_for_next_row(y,x)+1)
                    falling=true
                    falling_right=true
                end
            end
        end)
        if falling_right then
            yield_n_times(2)
        end
    end
end

function find_blocks_to_delete()
    local blocks_to_delete = {} -- y,x pairs
    for_all_tiles(function(y,x)
        local current_piece = board[y][x]
        if current_piece ~= empty then
            local one_row_up_x = x_for_next_row(y, x)

            -- square!
            if current_piece == board[y+1][one_row_up_x] and current_piece == board[y+1][one_row_up_x+1] and current_piece == board[y+2][x] then
                add(blocks_to_delete,{y=y,x=x})
                add(blocks_to_delete,{y=y+1,x=one_row_up_x})
                add(blocks_to_delete,{y=y+1,x=one_row_up_x+1})
                add(blocks_to_delete,{y=y+2,x=x})
            end
            -- line going left!
            if current_piece == board[y+1][one_row_up_x] and current_piece == board[y+2][x-1] then
                add(blocks_to_delete,{y=y,x=x})
                add(blocks_to_delete,{y=y+1,x=one_row_up_x})
                add(blocks_to_delete,{y=y+2,x=x-1})
            end
            -- line going right!
            if current_piece == board[y+1][one_row_up_x+1] and current_piece == board[y+2][x+1] then
                add(blocks_to_delete,{y=y,x=x})
                add(blocks_to_delete,{y=y+1,x=one_row_up_x+1})
                add(blocks_to_delete,{y=y+2,x=x+1})
            end
        end
    end)
    return blocks_to_delete
end

function hit_bottom()
    hard_dropping = false
    combo_size = 0
    y_shift -= shimmy_coefficient/2
    sfx(11)

    blocks_clearing=cocreate(function()
        yield_n_times(3)
        let_pieces_settle()

        yield_n_times(3)
        local cleared_things = true -- todo this is a lie at this moment
        local scored_this_turn = 0
        while cleared_things do
            cleared_things = false

            local blocks_to_delete = find_blocks_to_delete()
            local cleared_this_iteration = 0 --todo this makes cleared_things pointless
            for b in all(blocks_to_delete) do
                cleared_things = true
                if board[b.y][b.x] ~= empty then
                    cleared_this_iteration += 1
                    particles_for_block_clear(b.y, b.x, board[b.y][b.x])
                end
                board[b.y][b.x] = empty
            end

            if cleared_things then
                combo_size += 1
                local combo_multiplier = mid(1, combo_size, 4)
                cleared += cleared_this_iteration
                scored_this_turn += combo_multiplier * calculate_points_scored(cleared_this_iteration, level)
                if #blocks_to_delete < 4 then
                    small_clear_sound()
                else
                    big_clear_sound()
                end
            end

            yield_n_times(3)
            if cleared_things then
                let_pieces_settle()
            end
        end

        if combo_size > 1 then
            drawing_combo_text = cocreate(draw_combo_text)
            yield_n_times(2)
            combo_reward_sound()
        end

        score += scored_this_turn
        level = flr(cleared/30)
        if level == 15 then
            game_state = "won"
            music(10)
        else
            new_player_quad()
        end
    end)
end
-- end clearing-blocks.lua
-- begin audio.lua
-->8
-- audio
function small_clear_sound()
    local clear_sounds = {0, 1, 2}
    music(clear_sounds[flr(rnd(#clear_sounds)) + 1])
end

function big_clear_sound()
    local clear_sounds = {3, 4, 5}
    music(clear_sounds[flr(rnd(#clear_sounds)) + 1])
end

function combo_reward_sound()
    local combo_sounds = {6, 7, 8, 9}
    music(combo_sounds[flr(rnd(#combo_sounds)) + 1])
end

function move_sound() sfx(flr(rnd(number_of_sounds + 1))) end

-- end audio.lua
-- begin movement.lua
-->8
-- block movement

function x_for_next_row(current_y, current_x)
    if is_odd(current_y) then return current_x end
    return current_x - 1
end

-- Moving blocks
function rotate_clockwise()
    local p0 = player:player0()
    local p1 = player:player1()
    local p2 = player:player2()
    local p3 = player:player3()

    local tmp = board[p0.y][p0.x]
    board[p0.y][p0.x] = board[p2.y][p2.x]
    board[p2.y][p2.x] = board[p3.y][p3.x]
    board[p3.y][p3.x] = board[p1.y][p1.x]
    board[p1.y][p1.x] = tmp
end

function rotate_counter_clockwise()
    local p0 = player:player0()
    local p1 = player:player1()
    local p2 = player:player2()
    local p3 = player:player3()

    local tmp = board[p0.y][p0.x]
    board[p0.y][p0.x] = board[p1.y][p1.x]
    board[p1.y][p1.x] = board[p3.y][p3.x]
    board[p3.y][p3.x] = board[p2.y][p2.x]
    board[p2.y][p2.x] = tmp
end

function move_down(next_y, next_x)
    local p0 = player:player0()
    local p1 = player:player1()
    local p2 = player:player2()
    local p3 = player:player3()

    local next_y = player.y - 2
    local next_x = player.x

    if next_y > 0 and board[next_y][next_x] == empty then
        move_piece(p0.y, p0.x, next_y, next_x)
        move_piece(p1.y, p1.x, next_y + 1, p1.x)
        move_piece(p2.y, p2.x, next_y + 1, p2.x)
        move_piece(p3.y, p3.x, next_y + 2, next_x)

        player.x = next_x
        player.y = next_y
    elseif next_y < 0 then
        hit_bottom()
    else
        local next_action = hit_bottom
        if can_move_left(p0, p1) and can_move_right(p0, p2) then
            if last_direction_moved == "right" then
                next_action = move_right
            else
                next_action = move_left
            end
        elseif can_move_left(p0, p1) then
            next_action = move_left
        elseif can_move_right(p0, p2) then
            next_action = move_right
        end
        next_action()
    end
end

function move_left()
    local p0 = player:player0()
    local p1 = player:player1()
    local p2 = player:player2()
    local p3 = player:player3()

    local next_y = p0.y - 1
    local next_x = x_for_next_row(p0.y, p0.x)
    local one_row_up_x = x_for_next_row(p0.y + 1, next_x)

    if can_move_left(p0, p1) then
        move_piece(p0.y, p0.x, next_y, next_x)
        move_piece(p1.y, p1.x, next_y + 1, one_row_up_x)
        move_piece(p2.y, p2.x, next_y + 1, one_row_up_x + 1)
        move_piece(p3.y, p3.x, next_y + 2, next_x)

        player.x = next_x
        player.y = next_y
        last_direction_moved = "left"
        return true
    elseif not can_move_right(p0, p2) then
        hit_bottom()
        return false
    end
end

function can_move_left(p0, p1)
    return p0.y - 1 > 0 and block_can_fall_left(p0.y, p0.x) and
               block_can_fall_left(p1.y, p1.x)
end

function block_can_fall_left(old_y, old_x)
    local next_x = x_for_next_row(old_y, old_x)
    local next_y = old_y - 1
    if next_y < 1 or next_x < 1 then return false end

    return board[next_y][next_x] == empty
end

function move_right() -- todo combine into one method with move_left
    local p0 = player:player0()
    local p1 = player:player1()
    local p2 = player:player2()
    local p3 = player:player3()

    local next_y = p0.y - 1
    local next_x = x_for_next_row(p0.y, p0.x) + 1
    local one_row_up_x = x_for_next_row(p0.y + 1, next_x)

    if can_move_right(p0, p2) then
        move_piece(p0.y, p0.x, next_y, next_x)
        move_piece(p1.y, p1.x, next_y + 1, one_row_up_x)
        move_piece(p2.y, p2.x, next_y + 1, one_row_up_x + 1)
        move_piece(p3.y, p3.x, next_y + 2, next_x)

        player.x = next_x
        player.y = next_y
        last_direction_moved = "right"
        return true
    elseif not can_move_left(p0, p1) then
        hit_bottom()
        return false
    end
end

function can_move_right(p0, p2)
    return p0.y - 1 > 0 and block_can_fall_right(p0.y, p0.x) and
               block_can_fall_right(p2.y, p2.x)
end

function block_can_fall_right(old_y, old_x)
    local next_x = x_for_next_row(old_y, old_x) + 1
    local next_y = old_y - 1
    if next_y < 1 or next_x > board_width then return false end
    return board[next_y][next_x] == empty
end

function move_piece(old_y, old_x, new_y, new_x)
    board[new_y][new_x] = board[old_y][old_x]
    board[old_y][old_x] = empty
end

-- end movement.lua
-- begin fancy-printing.lua
-->8
-- fancy printing

function print_in_box(message, x, y)
    local pico8_letter_width = 4
    local message_width_px = #message * pico8_letter_width
    local box_left = x - message_width_px / 2 - pico8_letter_width
    local box_right = x + message_width_px / 2 + 2
    local box_top = y - pico8_letter_width
    local box_bottom = y + pico8_letter_width * 2
    local box_color = 6

    rectfill(box_left + 1, box_top + 1, box_right - 1, box_bottom - 1, box_color)
    rectfill(box_left, box_top + 2, box_right, box_bottom - 2, box_color)

    print(message, x - message_width_px / 2, y, 1)

end

function centered_print(text, x, y, col, outline_col)
    outlined_print(text, x - #text * 2, y, col, outline_col)
end

function outlined_print(text, x, y, col, outline_col)
    print(text, x - 1, y, outline_col)
    print(text, x + 1, y, outline_col)
    print(text, x, y - 1, outline_col)
    print(text, x, y + 1, outline_col)

    print(text, x, y, col)
end

-- end fancy-printing.lua
-- begin game-objects.lua
-->8
-- game object factories

-- player is represented as the bottom of the falling quad
--     player3
-- player1  player2
--     player0
function new_player_quad()
    player = {
        y = board_height - 2,
        x = 4,
        player0 = function(self) return {x = self.x, y = self.y} end,
        player1 = function(self) -- todo get all of these back in one method call
            return {x = x_for_next_row(self.y, self.x), y = self.y + 1}
        end,
        player2 = function(self)
            return {x = x_for_next_row(self.y, self.x) + 1, y = self.y + 1}
        end,
        player3 = function(self) return {x = self.x, y = self.y + 2} end,
        is_player_piece = function(self, y, x)
            local p3 = self:player3()
            local p2 = self:player2()
            local p1 = self:player1()
            local p0 = self:player0()

            return (x == p0.x or x == p1.x or x == p2.x or x == p3.x) and
                       (y == p0.y or y == p1.y or y == p2.y or y == p3.y)
        end
    }
    -- todo add function to player to multi return all current pieces
    local p3 = player:player3()
    local p2 = player:player2()
    local p1 = player:player1()
    local p0 = player:player0()

    if board[p3.y][p3.x] ~= empty or board[p2.y][p2.x] ~= empty or
        board[p1.y][p1.x] ~= empty or board[p0.y][p0.x] ~= empty or
        (not block_can_fall_left(p1.y, p1.x) and
            not block_can_fall_right(p2.y, p2.x)) then
        sfx(24)
        game_state = "gameover"
    end
    board[p3.y][p3.x] = next_quad.p3
    board[p2.y][p2.x] = next_quad.p2
    board[p1.y][p1.x] = next_quad.p1
    board[p0.y][p0.x] = next_quad.p0

    speed_timer = 0

    make_next_quad()
end

function player_quad_shadow()
    local s0 = player:player0()
    local s1 = player:player1()
    local s2 = player:player2()
    local s3 = player:player3()

    local next_y = s0.y - 2
    local next_x = s0.x

    while next_y > 0 and board[next_y][next_x] == empty do
        s0 = {y = next_y, x = next_x}
        s1.y = next_y + 1
        s2.y = next_y + 1
        s3 = {y = next_y + 2, x = next_x}

        next_y = s0.y - 2
        next_x = s0.x
    end

    return {
        s0 = s0,
        s1 = s1,
        s2 = s2,
        s3 = s3,
        is_in_shadow = function(self, y, x)
            s0 = self.s0
            s1 = self.s1
            s2 = self.s2
            s3 = self.s3
            return (y == s0.y and x == s0.x) or (y == s1.y and x == s1.x) or
                       (y == s2.y and x == s2.x) or (y == s3.y and x == s3.x)
        end,
        get_corresponding_sprite = function(self, y, x)
            local s0 = self.s0
            local s1 = self.s1
            local s2 = self.s2
            local s3 = self.s3
            local p0 = player:player0()
            local p1 = player:player1()
            local p2 = player:player2()
            local p3 = player:player3()

            if y == s0.y and x == s0.x then
                return board[p0.y][p0.x]
            elseif y == s1.y and x == s1.x then
                return board[p1.y][p1.x]
            elseif y == s2.y and x == s2.x then
                return board[p2.y][p2.x]
            end
            return board[p3.y][p3.x]
        end,
        draw_slide_indicator_arrow = function(self)
            if currently_clearing_blocks() or game_state ~= "playing" then
                return
            end

            local s0 = self.s0
            local s1 = self.s1
            local s2 = self.s2

            local next_x = x_for_next_row(s0.y, s0.x)
            local next_y = s0.y - 1
            local arrow_sprite = 6

            local right = can_move_right(s0, s2)
            local left = can_move_left(s0, s1)

            -- todo this logic is duplicated somewhere else, could dedupe to save tokens
            if left and right then
                if last_direction_moved == "right" then
                    -- if they're moving right, they would slide right
                    left = false
                else
                    -- and vice versa
                    right = false
                end
            end

            if right then
                -- get screen position to draw arrow
                local y_pos, x_pos = get_screen_position_for_block(next_y,
                                                                   next_x + 1)
                spr(arrow_sprite, x_pos + 2, y_pos - 2)
            end

            if left then
                -- get screen position to draw arrow
                local y_pos, x_pos = get_screen_position_for_block(next_y,
                                                                   next_x)
                spr(arrow_sprite, x_pos - 3, y_pos - 2, 1, 1, true, false)
            end
        end
    }
end

function make_next_quad()
    local p0 = random_block()
    local p1 = random_block()
    local p2 = random_block()
    local p3 = random_block()
    while p0 == p1 and p1 == p2 and p2 == p3 do p3 = random_block() end
    next_quad = {p0 = p0, p1 = p1, p2 = p2, p3 = p3}
end

function random_block()
    local val = flr(rnd(4))
    if val == 0 then
        return white_block
    elseif val == 1 then
        return red_block
    elseif val == 2 then
        return yellow_block
    end
    return blue_block
end

function new_board()
    local grid = {}
    local y
    local x
    for y = 1, board_height do
        grid[y] = {}

        for x = 1, board_width do
            local piece = empty

            if wall_here(y, x) then piece = wall end

            grid[y][x] = piece
        end
    end
    return grid
end

function wall_here(y, x)
    -- TODO should still be cleaned up more
    return (row_at_beginning_or_end(y, 1) and x ~= 4) or
               (row_at_beginning_or_end(y, 2) and (x < 4 or 5 < x)) or
               (row_at_beginning_or_end(y, 3) and (x < 3 or 5 < x)) or
               (row_at_beginning_or_end(y, 4) and (x < 3 or 6 < x)) or
               (row_at_beginning_or_end(y, 5) and (x < 2 or 6 < x)) or
               (row_at_beginning_or_end(y, 6) and (x < 2 or 7 < x)) or
               (is_odd(y) and x == board_width)
end

function row_at_beginning_or_end(real, expected)
    return real == expected or real == board_height - expected + 1
end

-- end game-objects.lua
-- begin init.lua
-->8
-- init functions

function _init()
    game_state = "menu"
    particles={}

    -- config
    turn_on_shadow()
    standard_rotation_mode()
    turn_off_millisecond_display()
end

function start_game()
    board = new_board()
    game_state = "playing"
    setup_palette()
    make_next_quad()
    new_player_quad()
    last_direction_moved = "right"
    speed_timer = 0
    seconds_timer = 0
    seconds_elapsed = 0
    speed = 27
    cleared = 0
    combo_size = 0
    level = 0
    score = 0
    x_shift = 0
    y_shift = 0
    hard_dropping = false
end

-- end init.lua
-- begin update.lua
-->8
-- update

function _update()
    local p
    for p in all(particles) do
        p:update()
        if p:is_expired() then
            del(particles, p)
        end
    end
    if game_state == "menu" or game_state == "gameover" or game_state == "won" then
        update_menu()
    elseif game_state == "playing" then
        update_game()
    end
end


function update_game()
    update_timers()

    if currently_clearing_blocks() then
        coresume(blocks_clearing)
    elseif hard_dropping then
        handle_rotational_input()
        move_down()
    else
        if speed_timer >= (speed - level) then
            tick()
            speed_timer = 0
        end
        handle_directional_input()
        handle_rotational_input()
    end
end

function update_timers()
    speed_timer += 1
    seconds_timer += 1

    if seconds_timer == 30 then
        seconds_elapsed += 1
        seconds_timer = 0
    end
end

function update_menu()
    if loading then
        if current_fade_perc > 1.5 then
            start_game()
            loading=false
        end
    elseif btn(4) and btn(5) then
        music(7)
        loading=true
    end
end

function tick()
    move_down()
end

function handle_directional_input()
    local just_moved=false
    if btnp(0) then
        just_moved=move_left()
        x_shift=shimmy_coefficient
    elseif btnp(1) then
        just_moved=move_right()
        x_shift=-shimmy_coefficient
    elseif btnp(3) then
        hard_dropping=true
        move_down()
        y_shift-=shimmy_coefficient
    end
    if just_moved then
        move_sound()
    end
end

function handle_rotational_input()
    if not swap_rotation_buttons then
        if btnp(4) then
            rotate_counter_clockwise()
            move_sound()
        elseif btnp(5) then
            rotate_clockwise()
            move_sound()
        end
    else
        if btnp(5) then
            rotate_counter_clockwise()
            move_sound()
        elseif btnp(4) then
            rotate_clockwise()
            move_sound()
        end
    end
end
-- end update.lua
-- begin draw.lua
-->8
-- draw functions

function _draw()
    cls()
    camera()

    if game_state == "menu" then
        draw_menu()
    else
        draw_hud()
        if drawing_combo_text and costatus(drawing_combo_text) ~= dead then
            coresume(drawing_combo_text)
        end

        doshake()
        draw_board()
    end

    local p
    for p in all(particles) do
        p:draw()
    end

    local board_center = board_left+40
    if game_state == "gameover" then
        print_in_box("game over",board_center, 44)
        print_in_box("\x8e + \x97 to try again  ",board_center, 99)
    elseif game_state == "won" then
        print_in_box("you win!!!",board_center, 44)
        print_in_box("\x8e + \x97 to play again  ",board_center, 99)
    end
end

function draw_menu()
    -- halo
    pal(7,1)
    sspr(1,8,121,25,4,45)
    sspr(1,8,121,25,5,44)
    sspr(1,8,121,25,6,45)
    sspr(1,8,121,25,5,46)
    pal()

    -- real title
    sspr(1,8,121,25,5,45)

    centered_print("press \x8e + \x97 to begin  ", 64, 103,7,1)

    if loading then
        fadepal(current_fade_perc, 1)
        current_fade_perc+=fade_speed
    end
end

-- The board is organized with 1,1 as the bottom left corner
-- every other row is drawn shifted right by a half position
-- the diamond shape is made by setting the out-of-bounds spaces to "wall"
function draw_board()
    local shadow
    if player then
        shadow = player_quad_shadow()
    end

    for_all_tiles(function(y,x)
        local y_loc, x_loc = get_screen_position_for_block(y,x)
        if board[y][x] ~= wall then
            local sprite = board[y][x]

            if sprite == empty and display_shadow and shadow and shadow:is_in_shadow(y,x) and not currently_clearing_blocks() then
                -- switch block colors to block shadow colors
                pal()
                pal(12, 3)
                pal(8, 2)
                pal(7, 6)
                pal(10, 9)
                sprite = shadow:get_corresponding_sprite(y,x)
            end


            sspr(sprite,0,sprite_size,sprite_size,x_loc,y_loc)
            pal()
            setup_palette()
        end
    end)

    if display_shadow and shadow then
        shadow:draw_slide_indicator_arrow()
    end
    draw_board_outline()
end

function draw_board_outline()
    -- draw outline of board
    color(board_outline_color)
    local center_point_x=board_left+38
    local bottom_corner_y=bottom-28
    local upper_corner_y=bottom-79
    local bottom_point_y=bottom+5

    -- left side
    local left_side_line_x=board_left+5
    line(center_point_x, bottom_point_y, left_side_line_x, bottom_corner_y)
    line(left_side_line_x, upper_corner_y)
    line(center_point_x, bottom-board_height*piece_height-4)

    -- right side
    local right_side_line_x=board_left+board_width*piece_width+8
    line(center_point_x+1, bottom-board_height*piece_height-4, right_side_line_x, upper_corner_y)
    line(right_side_line_x, bottom_corner_y)
    line(center_point_x+1, bottom_point_y)
end

function draw_hud()
    local right_side_x=board_left+84
    local y_loc=8

    color(7)
    print("time",right_side_x, y_loc)
    print(display_time(), right_side_x, y_loc+8)

    print("level", right_side_x, y_loc+24)
    print(level.."/15", right_side_x, y_loc+32)

    print("cleared",right_side_x,y_loc+48)
    print(cleared, right_side_x,y_loc+56)

    print("score", right_side_x, y_loc+72)
    print(score, right_side_x, y_loc+80)

    local next_quad_y = y_loc+96
    local next_quad_x = right_side_x +piece_width/2
    sspr(next_quad.p3,0,sprite_size,sprite_size,next_quad_x,next_quad_y)
    sspr(next_quad.p2,0,sprite_size,sprite_size,next_quad_x+piece_width/2,next_quad_y+piece_height)
    sspr(next_quad.p1,0,sprite_size,sprite_size,next_quad_x-piece_width/2,next_quad_y+piece_height)
    sspr(next_quad.p0,0,sprite_size,sprite_size,next_quad_x,next_quad_y+piece_height*2)
end

function draw_combo_text()
    local x_loc=board_left-5
    local y_loc=8
    local message = combo_size.."x combo!"

    local i
    for i=1,20 do
        if i % 5 == 0 then
            y_loc -= 1
        end
        print(message,x_loc,y_loc,11)
        yield()
    end
end

-- return time elapsed in format mm:ss
function display_time()
    local minutes = flr(seconds_elapsed / 60)
    local seconds_remainder = seconds_elapsed % 60
    local display_minutes = tostr(minutes)
    if #display_minutes < 2 then
        display_minutes = "0" .. display_minutes
    end
    local display_seconds = tostr(seconds_remainder)
    if #display_seconds < 2 then
        display_seconds = "0" .. display_seconds
    end

    local t = display_minutes .. ":" .. display_seconds
    if display_milliseconds then
        t = t .. "." .. 33 * seconds_timer
    end

    return t
end
-- end draw.lua
__gfx__
00000000007700000088000000aa000000cc00000011000000000000000000000000000000000000000000000000000000000000000000000000000000000000
0000000007007000080080000aaaa0000cccc0000111100000000000000000000000000000000000000000000000000000000000000000000000000000000000
007007007000070080880800aaaaaa00cc00cc001111110000606000000000000000000000000000000000000000000000000000000000000000000000000000
000770007000070080880800aaaaaa00cc00cc001111110000066000000000000000000000000000000000000000000000000000000000000000000000000000
0007700007007000080080000aaaa0000cccc0000111100000666000000000000000000000000000000000000000000000000000000000000000000000000000
00700700007700000088000000aa000000cc00000011000000000000000000000000000000000000000000000000000000000000000000000000000000000000
00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
07700000000007000000000000070000000000077000000000000700000000000700000000700000700000000000000007000000700000000070000000000000
07070000000007000000000000700000000000700700000000000700000000000700000000700000700000000000000007000000700000000070000000000000
07007000000007000000000007000000000007000070000000000700000000000700000000700000770000000000000077000000700000000707000000000000
07000700000007000000000070000000000070000007000000000700000000000700000000700000770000000000000077000000700000000707000000000000
07000070000007000000000700000000000700000000700000000700000000000700000000700000707000000000000707000000700000000707000000000000
07000007000007000000007000000000007000000000070000000700000000000700000000700000707000000000000707000000700000000707000000000000
07000007000007000000070000000000070000000000007000000700000000000700000000700000700700000000007007000000700000007000700000000000
07000070000007000000700000000000700000000000000700000700000000000700000000700000700700000000007007000000700000007000700000000000
07000700000007000007000000000007000000000000000070000700000000000700000000700000700070000000070007000000700000007000700000000000
07007000000007000007000000000007000000000000000070000700000000000700000000700000700070000000070007000000700000070000070000000000
07070000000007000000700000000000700000000000000700000700000000000700000000700000700007000000700007000000700000070000070000000000
07700000000007000000070000000000070000000000007000000700000000000700000000700000700007000000700007000000700000077777770000000000
07000000000007000000007000000000007000000000070000000700000000000700000000700000700000700007000007000000700000700000007000000000
07000000000007000000000700000000000700000000700000000700000000000700000000700000700000700007000007000000700000700000007000000000
07000000000007000000000070000000000070000007000000000700000000000770000007700000700000070070000007000000700000700000007000000000
07000000000007000000000007000000000007000070000000000700000000000070000007000000700000070070000007000000700000700000007000000000
07000000000007000000000000700000000000700700000000000700000000000077000077000000700000007700000007000000700007000000000700000000
07000000000007000000000000070000000000077000000000000777777770000007777770000000700000000000000007000000700007000000000700000000
__label__
00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
00000000000000000000000000000000000000000000000000000000000000600000000000000000000000000000000000000000000000000000000000000000
00000000000000000000000000000000000000000000000000000008000000000000000000000000000000000000000000000000000000000000000000000000
00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
00000000000000000000000000000000000000000000000000070000000000000000000000000000000000000000000000000000000000000000000000000000
0000000000000000000000000000000000000000000000dd00000000000000000000000000000000000000000000000000000000000000000000000000000000
000jjj0j0j0000008j70jj0j7j0jjj00jj00j00000000d00d0000000000000000000000000000000000000000000000000000000000000000000000000000000
00000j0j0j00000j000j0j0jjj0j0j0j0j00j0000000d0000d000000000000000000000000000000000000000000000000000000000000000000000000000000
000jjj00j000070j000j0j0j0j0jj00j0j00j000000d000000d00000000000000000000000000000000000000000777077707770777000000000000000000000
000j000j0j00006j000j0j0j0j0j0j0j0j00000000d000hh000d0000000000000000000000000000000000000000070007007770700000000000000000000000
000jjj0j0j000000jj0jj00j0j0jj70jj000j0000d000hhhh000d000000000000000000000000000000000000000070007007070770000000000000000000000
0000000000000000000000000000000000000000d000hhhhhh077d00000000000000000000000000000000000000070007007070700000000000000000000000
000000000070000000000000000000000000000d0000hhhhhh0000d0000000000000000000000000000000000000070077707070777000000000000000000000
00000007000000000000000000000000000000d000hh0hhhh0hh000d000000000000000000000000000000000000000000000000000000000000000000000000
0000000000000000000000000000000000000d000hhhh0hh0hhhh000d00000000000000000000000000000000000000000000000000000000000000000000000
000000070000000000000000000000000000d000hhhhhh00hhhhhh000d0000000000000000000000000000000000000000000000000000000000000000000000
00000000000000000000000000000000000d0000hhhhhh007hhhhh0000d0000000000000000h0000000000000000777077700000777070700000000000000000
0000000000000000000000000000000000d000hh0hhhh0770hhhh0hh000800000000000007000000000000000000707000700700707070700000000000000000
000000000000000000000000000000000d000hhhh0hh070070hh0hhhh000d0000000000000000000000000000000707077700000707077700000000000000000
00000000000000000000000000000000d000hhhhhh0070000700hhhhhh000d000000000000000000000000000000707070000700707000700000000000000000
0000000000000000000000000000000d0000hhhhhh0070000700hhhhhh0000d00000000000000070000000000000777077700000777000700000000000000000
000000000000000000000000000000d000hh0hhhh0cc070070aa0hhhh0hh000d0000000000000000000000000000000000000000000000000000000000000000
00000000000000000000000000000d000hhhh0hh0cccc0770aaaa0hh0hhhh000d000000000007000000000000000000000000000000000000000000000000000
0000000000000000000000000000d000hhhhhh00cc00cc00aaaaaa00hhhhhh000d00000000000000000000000000000000000000000000000000000000000000
000000000000000000000000000d0000hhhhhh00cc00cc00aaaaaa00hhhhhh0000d0000000007000070000000000000000000000000000000000000000000000
00000000000000000000000000d000hh0hhhh0hh0cccc0880aaaa0hh0hhhh0hh000d000000007000000000000000000000000000000000000000000000000000
0000000000000000000000000d000hhhh0hh0hhhh0cc080080aa0hhhh0hh0hhhh000d00000000000070000000000000000000000000000000000000000000000
000000000000000000000000d000hhhhhh00hhhhhh0080880800hhhhhh00hhhhhh000d0000070770000000000000000000000000000000000000000000000000
00000000000000000000000d0000hhhhhh00hhhhhh0080880800hhhhhh00h7hhhh0000d000000000000000000000000000000000000000000000000000000000
0000000000000000000000d000hh0hhhh0hh0hhhh0ss080080nn0hhhh0cc7hhhh0hh700d00060070000000000000000000000000000000000000000000000000
000000000000000000000d000hhhh0hh0hhhh0hh0ssss0880nnnn0hh0cccc0hh0hhhh000d0700000070000000000000000000000000000000000000000000000
00000000000000000000d000hhhhhh00hhhhhh00ss00ss00nnnnnn00cc00cc00hhhhhh000d700000000000000000000000000000000000000000000000000000
0000000000000000000d0000hhhhhh00hhhhhh00ss00ss00nnnnnn00cc00cc00hhhhhh0000d70000000000000000700077707070777070000000000000000000
000000000000000000d000hh0hhhh0hh0hhhh0hh0ssss0oo0nnnn0cc0cccc0880hhh60hh000d0000000000000000700070007070700070000000000000000000
00000000000000000d000hhhh0hh0hhhh0hh0hhhh0ss0o00o0nn0cccc0cc0800o0hh0hhhh000d000000000000000700077007070770070000000000000000000
0600000000000000d000hhhhhh00hhhhhh00hhhhhh00o0oo0o00cc00cc0080880800hhhhhh000d00000000000000700070007770700070000000000000000000
000000000000000d0000hhhhhh00hhhhhh00hhhhhh00o0oo0o00cc00cc0080880800hhhhhh0000d0000000000000777077700700777077700000000000000000
07000000000000d000hh0hhhh0aa0hhhh0770hhh606h0o00o0880cccc0aa080080770hhhh07h000d000000000000000000000000000000000000000000000000
0000000000000d000hhhh0hh0aaaa0hh070070hh66hhh0oo080080cc0aaaa088070070hh0hhhh000d00000000000000000000000000000000000000000000000
0000000000000d00hhhhhh00aaaaaa0070000700666hhh0080880800aaaaaa0070000700hh7hhh00d00000000000000000000000000000000000000000000000
0000000000000d00hhhhhh00aaaaaa0070000700hhhhhh0080880800aaaaaa0070000700hhhhhh00d00000000000707000707700777000000000000000000000
0000000000000d000hhhh0cc0aaaa088070070770hhhh0aa080080770aaaa088070070770hhhh000d00000000000707007000700700000000000000000000000
0000000000000d0000hh0cccc0aa08008077070070hh0aaaa088070070aa08008077070070hh0000d00000000000777007000700777000000000000000000000
0000000000000d000000cc00cc008088080070000700aaaaaa007000070080880800700007000000d00000000000007007000700007000000000000000000000
0000000000000d000000cc00cc008088080070000700aaaaaa007000070080880800700007000000d00000000000007070007770777000000000000000000000
0000000000000d0000880cccc0aa080080aa070070880aaaa0cc070070aa08008088070078aa0000d00000000000000000000000000000000000000000000000
0000000000000d00080080cc0aaaa0880aaaa077080080aa0cccc0770aaaa088080080770aaaa000d00000000000000000000000000000000000000000000000
0000000000000d0080880800aaaaaa00aaaaaa0080880800cc00cc00aaaaaa0080880800aaaaaa00d00000000000000000000000000000000000000000000000
0000000000000d0080880800aaaaaa00aaaaaa0080880800cc00cc00aaaaaa0080880800aaaaaa00d00000000000000000000000000000000000000000000000
0000000000000d00080080770aaaa0cc0aaaa0aa080080aa0cccc0880aaaa0aa080080cc0aaaa000d00000000000000000000000000000000000000000000000
0000000000000d000088070070aa0cccc0aa0aaaa0880aaaa0cc080080aa0aaaa0880cccc0aa0000d00000000000000000000000000000000000000000000000
0000000000000d00000070000700cc00cc00aaaaaa00aaaaaa0080880800aaaaaa00cc00cc000000d07000000000000000000000000000000000000000000000
0000000000000d00000070000700cc00cc00aaaaaa00aaaaaa0080880800aaaaaa00cc00cc000000d00000000000000000000000000000000000000000000000
0000000000000d000077070070cc0cccc0aa0aaaa0880aaaa077080080cc0aaaa0770cccc0da0000d00000000000000000000000000000000000000000000000
0000000000000d00070070770cccc0cc0aaaa0aa080080aa070070880cccc0aa070070cc0aaaa000d00000000000000000000000000000000000000000000000
0000000000000d0070000700cc00cc00aaaaaa008088080070000700cc00cc0070000700aaaaaa00d00000000000000000000000000000000000000000000000
0000000000000d0070000700cc00cc00aaaaaa008088080070000700cc00cc0070000700aaaaaa00d00000000000077070007770777077707770770000000000
0000000000000d00070070770cccc0cc0aaaa0cc080080cc070070cc0cccc0cc0700d0880aaaa000d00000000000700070007000707070707000707000000000
0000000000000d000077070070cc0cccc0aa0cccc0880cccc0770cccc0cc0cccc077087080aa0000d00000000000700070007700777077007700707000000000
0000000000000d00000070000700cc00cc00cc00cc00cc00cc00cc00cc00cc00cc00d08808000000d00000000000700070007000707070707000707000000000
0000000000000d00000070000700cc00cc00cc00cc00cc00cc00cc00cc00cc00cc0h808808000000d00000000000077077707770707070707770777000000000
0070000000000d000088070070880cccc0770cccc0770cccc0aa0cccc0880cccc07a080080cc0000d00000000000000000000000000000000000000000000000
0000000000000d0008008077080080cc070070cc070070cc0aaaa0cc080080cc7aaaa0880cccc000d00000000000000000000000000000000000000000000000
0000000000000d0080880800808808007000070070000700aaaaaa0080880807aaaaaa00cc00cc00d00000000000000000000000000000000000000000000000
0000000000000d0080880800808808007000070070000700aaaaaa0080880800aaaaaa00cc00cc00d00000000000770077707770000000000000000000000000
0000000000000d00080080cc08008088070070cc070070770aaaa0aa080080aa0aaaahcc0cccc000d00000000000070000700070000000000000000000000000
0000000000000d0000880cccc088080080770cccc077070070aa0aaaa0880aaaa7aa0cccc0ccd000d00000000000070077707770000000000000000000000000
0000007000000d000000cc00cc0080880800cc00cc0070000700aaaaaa00aa7aaa00cc00cc000000d00000000000070070007000000000000000000000000000
0000000000000d000000cc00cc0080880800cc00cc0070000700aaaaaa00aaaaaa00cc00cc000000d00000000000777077707770000000000000000000000000
0000000000000d0000aa0cccc077080080aa0cccc077070070880aaaa0cc0aaaa0880cccc0880000d00000000000000000000000000000000000000000000000
0000000000000d000aaaa0cc070070880aaaa0cc07007077080080aa0cccodaa080080cc08008000d00000000000000000000000000000000000000000000000
0000000000000d00aaaaa60070000700aaaaaa007000070080880800cc00cc008088080080880800d00000000000000000000000000000000000000000000000
0000000000000d00aaaaaa0070000700aaaaaa0070000700808808707c00cc008088080080880800d00000000000000000000000000000000000000000000000
0000000000000d000aaaa077070070770aaaa08807007088080080770cccc088080080cc08008000d00000000000000000000000000000000000000000000000
0000000000000d0000aa07007077070070aa0800807708008788070070cc080080880cccc0880000d00000000000000000000000000000000000000000000000
0000070700000d000000700007007000070080880870708808007000070080880800cc00cc000000d00000000000000000000000000000000000000000000000
0000000000000d000000700007007000070080880800808808007700070080880800cc00cc000000d00000000000000000000000000000000000000000000000
0000000060000d0000880700707a070070aa0800807708078077070070aa080080770cccc0770000d00000000000000000000000000000000000000000000000
0000000000080d00080080770aaaa0770aaaa08807007088070070770aaaa088070070cc07007000d00000000000000000000000000000000000000000000000
0000000000000d0060880800aaaa6a00aaaaaa007000770070000700aaaaaa007000070070000700d00000000000000000000000000000000000000000000000
000000d000000d0087880800aaaaaa00aaaa7a007000070070000770aaaaaa007000070070000700d00000000000077007700770777077700000000000000000
0000000000000d00780070cc7aaaa0cc0aaaa0aa070070aa770070880aaaa088070070cc07007000d00000000000700070007070707070000000000000000000
0000000000000d0000870cccc0aa0cccc0aa0aaaa0770a7aa077080080aa080080770cccc0770000d00000000000777070007070770077000000000000000000
0000000000000d000000cc007c00cc00cc00aaaaa770aaaaaa008088080080880800cc00cc000000d00000000000007070007070707070000000000000000000
0000000000000d070000cc00cc00cc00cc00aaaaaa00aaaaaa008088080080880800cc00cc000000d00000000000770007707700707077700000000000000000
000000000000070000aa0cccc0aa0cccc0cc0aaaa0cc0aaaa0cc080080cc080080880cccc0aa0000d00000000000000000000000000000000000000000000000
0000000000000d000aaaa0cc0aaaa0c70cccc0aadcccc0aa7cccc0880cccc088080080cc0aaaa000d00000000000000000000000000000000000000000000000
0000000000000d00aaaaaa70aa7aaa00cc00cc00c700cc00cc00cc00cc00cc0080880800aaaaaa00d00000000000000000000000000000000000000000000000
0000000000000d00aaaaaa00aaaaaa00cc00cc00cc00cc00cc00cc00cc00cc0080880800aaaaaa00d00000000000707077707770000000000000000000000000
0000000000000d000aaaa0cc0aaaa0770cccc0aa0cccc0880cccc0880cccc0aa080080aa0aaaa000d00000000000707000700070000000000000000000000000
00000000000000d000aa0cccc0aa070070cc0aaaa0cc080080cc080080cc0aaaa0880aaaa0aa000d000000000000777000700770000000000000000000000000
000000000000000d0000cc00cc0070000700aaaaaa008088080080880800aaaaaa00aaaaaa0000d0000000000000007000700070000000000000000000000000
0000000000000000d000cc00cc0070000700aaaaaa008088080080880800aaaaaa00aaaaaa000d00000000000000007000707770000000000000000000000000
00000000000000000d000cccc077070070cc0aaaa07708008077080080770aaaa0770aaaa000d000000000000000000000000000000000000000000000000000
000000000000000000d000cc070070770cccc0aa0700708807007088070070aa070070aa000d0000000000000000000000000000000000000000000000000000
0000000000000000000d000070000700cc00cc007000070070000700700007007000070000d00000000000000000000000000000000000000000000000000000
00000000000000000000d00070000700cc00cc00700007007000070070000700700007000d000000000000000000000000000000000000000000000000000000
000000000000000000000d00070070aa0cccc0aa07007077070070cc0700707707007000d0000000000000000000000000000000000000000000000000000000
0000000000000000000000d000770aaaa0cc0aaaa077070070770cccc07707007077000d00000000000000000000000000000000000000000000000000000000
00000000000000000000000d0000aaaaaa00aaaaaa0070000700cc00cc007000070000d000000000000000000000000000000000000000000000000000000000
000000000000000000000000d000aaaaaa00aaaaaa0070000700cc00cc00700007000d0000000000000000000000000000000000000000000000000000000000
0000000000000000000000000d000aaaa0cc0aaaa0aa070070880cccc0aa07007000d00000000000000000000000000000000000000000000000000000000000
00000000000000000000000000d000aa0cccc0aa0aaaa077080080cc0aaaa077000d000000000000000000000000000000000000000000000000000000000000
000000000000000000000000000d0000cc00cc00aaaaaa0080880800aaaaaa0000d0000000000000000000000000000000000000000000000000000000000000
0000000000000000000000000000d000cc00cc00aaaaaa0080880800aaaaaa000d00000000000000000000000000000000cc0000000000000000000000000000
00000000000000000000000000000d000cccc0cc0aaaa077080080cc0aaaa000d00000000000000000000000000000000cccc000000000000000000000000000
000000000000000000000000000000d000cc0cccc0aa070070880cccc0aa000d00000000000000000000000000000000cc00cc00000000000000000000000000
0000000000000000000000000000000d0000cc00cc0070000700cc00cc0000d000000000000000000000000000000000cc00cc00000000000000000000000000
00000000000000000000000000000000d000cc00cc0070000700cc00cc000d00000000000000000000000000000000cc0cccc077000000000000000000000000
000000000000000000000000000000000d000cccc088070070cc0cccc000d00000000000000000000000000000000cccc0cc0700700000000000000000000000
0000000000000000000000000000000000d000cc080080770cccc0cc000d00000000000000000000000000000000cc00cc007000070000000000000000000000
00000000000000000000000000000000000d000080880800cc00cc0000d000000000000000000000000000000000cc00cc007000070000000000000000000000
000000000000000000000000000000000000d00080880800cc00cc000d00000000000000000000000000000000000cccc0770700700000000000000000000000
0000000000000000000000000000000000000d00080080aa0cccc000d0000000000000000000000000000000000000cc07007077000000000000000000000000
00000000000000000000000000000000000000d000880aaaa0cc000d000000000000000000000000000000000000000070000700000000000000000000000000
000000000000000000000000000000000000000d0000aaaaaa0000d0000000000000000000000000000000000000000070000700000000000000000000000000
0000000000000000000000000000000000000000d000aaaaaa000d00000000000000000000000000000000000000000007007000000000000000000000000000
00000000000000000000000000000000000000000d000aaaa000d000000000000000000000000000000000000000000000770000000000000000000000000000
000000000000000000000000000000000000000000d000aa000d0000000000000000000000000000000000000000000000000000000000000000000000000000
0000000000000000000000000000000000000000000d000000d00000000000000000000000000000000000000000000000000000000000000000000000000000
00000000000000000000000000000000000000000000d0000d000000000000000000000000000000000000000000000000000000000000000000000000000000
000000000000000000000000000000000000000000000d00d0000000000000000000000000000000000000000000000000000000000000000000000000000000
0000000000000000000000000000000000000000000000dd00000000000000000000000000000000000000000000000000000000000000000000000000000000
00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000

__sfx__
010f00000c05511005130051300514005130050e00500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500005
011000000c05500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
011000001105500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
010e00001505500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
011000001c05500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
011000001805500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
011000001105500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
011000001305500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
010e00001505500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500005
011000001d05500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500005000050000500005
011000001a05500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
011000000056300403005030050300503005030050300503005030050300503005030050300503005030050300503005030050300503005030050300503005030050300503005030050300503005030050300503
010c0000000550e0510e050000000c055000001104513041130400000010065000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
010c0000000550e0510e040000000c045000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
010c00000505513031130300000010035000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
010b0000000550e0510e050000000c04500000110451303113030000001005500000180651c0611c0600000018065000001306218060180501802018015000000000000000000000000000000000000000000000
010d00001101411020110201101500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
000c00001501415010130101301500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
010c00001701418020180221801200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
011000002401500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
011000002601200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
000f00002401200000007000000000700000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
000b0000150341502013025000000000000000000000000000000000000000000000000001302013025000000000000000000001c0201c0201c0101c015000000000000000000000000000000000000000000000
000b0000170341802018025000000000000000000000000000000000000000000000000001802018022000000000000000000002b0202b0102b01500000000000000000000000000000000000000000000000000
01130000000550e0510e0430d0410d0430c0410c0430b0410b0530a0510a053090510905308051080530705106051060510605106051060511205113053000001604217060000000000000000000000000000000
__music__
04 00021344
04 0a085844
04 02035344
04 00050715
04 01020315
04 03050415
04 10111244
04 0d111244
04 0e111244
04 10110d44
04 0f161744
