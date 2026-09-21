/*
 * The two string-driven sub-languages of Microsoft BASIC: PLAY's music macro
 * language and DRAW's turtle commands, plus SOUND and BEEP.
 */
#include "av.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>

#include "platform.h"
#include "screen.h"

#define BACKGROUND_QUEUE_MS 8000

typedef struct {
    const char *text;
    int length;
    int at;
} cursor_t;

static struct {
    int octave, length, tempo;
    int sounding_eighths;   /* MN 7, ML 8, MS 6 */
    bool background;
} music;

static struct {
    int x, y;
    int color;
    int scale;              /* quarter pixels per unit; 0 means 4 */
    double angle;           /* degrees, counter-clockwise */
} turtle;

void av_reset(void) {
    music.octave = 4;
    music.length = 4;
    music.tempo = 120;
    music.sounding_eighths = 7;
    music.background = false;
    turtle.x = scr.log_w / 2;
    turtle.y = scr.log_h / 2;
    turtle.color = -1;
    turtle.scale = 4;
    turtle.angle = 0;
    plat_tone_stop();
}

void av_set_position(int x, int y) {
    turtle.x = x;
    turtle.y = y;
}

void av_get_position(int *x, int *y) {
    *x = turtle.x;
    *y = turtle.y;
}

/* Waits until the queued sound is at most `allowed_ms` long. */
static int wait_for_queue(int allowed_ms, av_break_fn should_break) {
    while(plat_tone_queued_ms() > allowed_ms) {
        if(should_break && should_break())
            return AV_BREAK;
        plat_sleep_ms(2);
    }
    return AV_OK;
}

int av_wait_sound(av_break_fn should_break) {
    return wait_for_queue(0, should_break);
}

int av_sound(double hz, double ticks, av_break_fn should_break) {
    int status;

    if((hz != 0 && hz < 37) || hz > 32767 || ticks < 0 || ticks > 65535)
        return AV_ILLEGAL;
    if(ticks == 0) {
        plat_tone_stop();
        return AV_OK;
    }
    status = wait_for_queue(music.background ? BACKGROUND_QUEUE_MS : 0,
                            should_break);
    if(status != AV_OK)
        return status;
    plat_tone(hz, (int)(ticks * 1000.0 / 18.2 + 0.5));
    return AV_OK;
}

int av_beep(av_break_fn should_break) {
    return av_sound(800, 4.55, should_break);
}

static int peek(cursor_t *c) {
    while(c->at < c->length &&
          (c->text[c->at] == ' ' || c->text[c->at] == ';'))
        ++c->at;
    return c->at < c->length ? toupper((unsigned char)c->text[c->at]) : -1;
}

static bool read_number(cursor_t *c, int *value) {
    bool any = false;
    int n = 0;

    peek(c);
    while(c->at < c->length && isdigit((unsigned char)c->text[c->at])) {
        n = n * 10 + (c->text[c->at++] - '0');
        if(n > 100000)
            n = 100000;
        any = true;
    }
    *value = n;
    return any;
}

static int emit_note(int note, int length, cursor_t *c,
                     av_break_fn should_break) {
    double ms = 60000.0 / music.tempo * 4.0 / length;
    double add = ms / 2;
    int status, sounding;

    while(peek(c) == '.') {
        ++c->at;
        ms += add;
        add /= 2;
    }
    status = wait_for_queue(music.background ? BACKGROUND_QUEUE_MS : 0,
                            should_break);
    if(status != AV_OK)
        return status;
    if(note == 0) {
        plat_tone(0, (int)(ms + 0.5));
        return AV_OK;
    }
    sounding = (int)(ms * music.sounding_eighths / 8 + 0.5);
    /* Note 37 is middle C, the first note of octave 3. */
    plat_tone(261.6256 * pow(2.0, (note - 37) / 12.0), sounding);
    if((int)(ms + 0.5) > sounding)
        plat_tone(0, (int)(ms + 0.5) - sounding);
    return AV_OK;
}

int av_play(const char *text, int length, av_break_fn should_break) {
    static const int semitone[7] = {9, 11, 0, 2, 4, 5, 7};   /* A..G */
    cursor_t c = {text, length, 0};
    int ch, n, status;

    while((ch = peek(&c)) >= 0) {
        ++c.at;
        if(ch >= 'A' && ch <= 'G') {
            int note = music.octave * 12 + semitone[ch - 'A'] + 1;
            int note_length = music.length;
            int next = peek(&c);

            if(next == '#' || next == '+') {
                ++note;
                ++c.at;
            }
            else if(next == '-') {
                --note;
                ++c.at;
            }
            if(read_number(&c, &n)) {
                if(n < 1 || n > 64)
                    return AV_ILLEGAL;
                note_length = n;
            }
            if(note < 1 || note > 84)
                return AV_ILLEGAL;
            status = emit_note(note, note_length, &c, should_break);
            if(status != AV_OK)
                return status;
        }
        else if(ch == 'N') {
            if(!read_number(&c, &n) || n > 84)
                return AV_ILLEGAL;
            status = emit_note(n, music.length, &c, should_break);
            if(status != AV_OK)
                return status;
        }
        else if(ch == 'P' || ch == 'R') {
            n = music.length;
            if(read_number(&c, &n) && (n < 1 || n > 64))
                return AV_ILLEGAL;
            status = emit_note(0, n, &c, should_break);
            if(status != AV_OK)
                return status;
        }
        else if(ch == 'O') {
            if(!read_number(&c, &n) || n > 6)
                return AV_ILLEGAL;
            music.octave = n;
        }
        else if(ch == '>') {
            if(music.octave < 6)
                ++music.octave;
        }
        else if(ch == '<') {
            if(music.octave > 0)
                --music.octave;
        }
        else if(ch == 'L') {
            if(!read_number(&c, &n) || n < 1 || n > 64)
                return AV_ILLEGAL;
            music.length = n;
        }
        else if(ch == 'T') {
            if(!read_number(&c, &n) || n < 32 || n > 255)
                return AV_ILLEGAL;
            music.tempo = n;
        }
        else if(ch == 'M') {
            int which = peek(&c);
            ++c.at;
            if(which == 'F')
                music.background = false;
            else if(which == 'B')
                music.background = true;
            else if(which == 'N')
                music.sounding_eighths = 7;
            else if(which == 'L')
                music.sounding_eighths = 8;
            else if(which == 'S')
                music.sounding_eighths = 6;
            else
                return AV_ILLEGAL;
        }
        else {
            return AV_ILLEGAL;
        }
    }
    if(!music.background)
        return wait_for_queue(0, should_break);
    return AV_OK;
}

static bool read_signed(cursor_t *c, int *value, bool *is_signed) {
    int sign = 1, next = peek(c);

    *is_signed = false;
    if(next == '+' || next == '-') {
        *is_signed = true;
        sign = next == '-' ? -1 : 1;
        ++c->at;
    }
    if(!read_number(c, value))
        return false;
    *value *= sign;
    return true;
}

int av_draw(const char *text, int length, int default_color) {
    cursor_t c = {text, length, 0};
    bool pen_up = false, come_back = false;
    int ch;

    if(turtle.color < 0)
        turtle.color = default_color;
    while((ch = peek(&c)) >= 0) {
        double dx = 0, dy = 0;
        bool move = false, absolute = false;
        int n = 1, n2;
        bool sign_x, sign_y;

        ++c.at;
        switch(ch) {
            case 'B': pen_up = true; continue;
            case 'N': come_back = true; continue;
            case 'U': dy = -1; move = true; break;
            case 'D': dy = 1; move = true; break;
            case 'L': dx = -1; move = true; break;
            case 'R': dx = 1; move = true; break;
            case 'E': dx = 1; dy = -1; move = true; break;
            case 'F': dx = 1; dy = 1; move = true; break;
            case 'G': dx = -1; dy = 1; move = true; break;
            case 'H': dx = -1; dy = -1; move = true; break;
            case 'M':
                if(!read_signed(&c, &n, &sign_x) || peek(&c) != ',')
                    return AV_ILLEGAL;
                ++c.at;
                if(!read_signed(&c, &n2, &sign_y))
                    return AV_ILLEGAL;
                dx = n;
                dy = n2;
                absolute = !sign_x;
                break;
            case 'C':
                if(!read_number(&c, &n))
                    return AV_ILLEGAL;
                turtle.color = n & 255;
                continue;
            case 'S':
                if(!read_number(&c, &n) || n > 255)
                    return AV_ILLEGAL;
                turtle.scale = n ? n : 4;
                continue;
            case 'A':
                if(!read_number(&c, &n) || n > 3)
                    return AV_ILLEGAL;
                turtle.angle = n * 90;
                continue;
            case 'T':
                if(peek(&c) != 'A')
                    return AV_ILLEGAL;
                ++c.at;
                if(!read_signed(&c, &n, &sign_x) || n < -360 || n > 360)
                    return AV_ILLEGAL;
                turtle.angle = n;
                continue;
            case 'P':
                if(!read_number(&c, &n) || peek(&c) != ',')
                    return AV_ILLEGAL;
                ++c.at;
                if(!read_number(&c, &n2))
                    return AV_ILLEGAL;
                scr_paint(turtle.x, turtle.y, n & 255, n2 & 255);
                continue;
            default:
                return AV_ILLEGAL;
        }
        if(move) {
            if(!read_number(&c, &n))
                n = 1;
            dx *= n;
            dy *= n;
        }
        {
            int to_x, to_y;
            if(absolute) {
                to_x = (int)dx;
                to_y = (int)dy;
            }
            else {
                const double radians = turtle.angle * 3.14159265358979 / 180;
                const double scale = turtle.scale / 4.0;
                const double rx = dx * cos(radians) + dy * sin(radians);
                const double ry = -dx * sin(radians) + dy * cos(radians);
                to_x = turtle.x + (int)lround(rx * scale);
                to_y = turtle.y + (int)lround(ry * scale);
            }
            if(!pen_up)
                scr_line(turtle.x, turtle.y, to_x, to_y, turtle.color);
            if(!come_back) {
                turtle.x = to_x;
                turtle.y = to_y;
            }
        }
        pen_up = come_back = false;
    }
    return AV_OK;
}
