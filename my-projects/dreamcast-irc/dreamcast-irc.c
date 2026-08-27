/*
 * Dreamcast IRC -- a small KallistiOS IRC client for irc.libera.chat.
 *
 * The application uses the Dreamcast BBA (or an emulator's BBA) for a plain
 * TCP IRC connection and a Maple keyboard for text entry. It joins
 * #netsplit automatically after registration.
 */

#include <kos.h>
#include <kos/version.h>

#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/keyboard.h>
#include <dc/video.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IRC_SERVER "irc.libera.chat"
#define IRC_PORT "6667"
#define IRC_CHANNEL "#netsplit"
#define APP_NAME "DCIRC"
#define APP_VERSION "0.1"

#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480
#define UI_COLUMNS 48
#define VISIBLE_LINES 13
#define HISTORY_LINES 96
#define MAX_CHANNELS 10
#define MAX_PAGES (MAX_CHANNELS + 1)
#define CHANNEL_NAME_MAX 50
#define LIST_DISPLAY_MAX 80
#define INPUT_MAX 240
#define IRC_WIRE_MAX 512
#define IRC_RECV_MAX 768

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);

typedef enum connection_state {
    CONN_OFFLINE,
    CONN_CONNECTING,
    CONN_REGISTERING,
    CONN_JOINING,
    CONN_JOINED
} connection_state_t;

typedef enum line_style {
    STYLE_CHAT,
    STYLE_SELF,
    STYLE_EVENT,
    STYLE_NOTICE,
    STYLE_ERROR
} line_style_t;

typedef struct history_line {
    char text[UI_COLUMNS + 1];
    line_style_t style;
} history_line_t;

typedef struct irc_page {
    char name[CHANNEL_NAME_MAX + 1];
    bool joined;
    history_line_t history[HISTORY_LINES];
    int history_start;
    int history_count;
    int scroll_offset;
} irc_page_t;

static irc_page_t pages[MAX_PAGES];
static int irc_page_count = 1;
static int active_page;
static char input_text[INPUT_MAX + 1];
static size_t input_length;
static connection_state_t connection_state = CONN_OFFLINE;
static int irc_socket = -1;
static char nickname[16];
static unsigned nickname_attempt;
static char receive_line[IRC_RECV_MAX];
static size_t receive_length;
static bool receive_overflow;
static bool running = true;
static bool screen_dirty = true;
static bool input_dirty;
static maple_device_t *keyboard;
static uint32_t previous_buttons;
static unsigned list_entries_received;
static unsigned list_entries_displayed;
static bool list_in_progress;
static bool nickname_rng_seeded;

static uint16_t rgb565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r & 0xf8) << 8) |
                      ((g & 0xfc) << 3) |
                      ((b & 0xf8) >> 3));
}

static bool irc_name_equals(const char *a, const char *b) {
    while(*a && *b) {
        char ca = *a++;
        char cb = *b++;

        if(ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if(cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if(ca != cb)
            return false;
    }
    return *a == '\0' && *b == '\0';
}

static bool valid_channel_name(const char *channel) {
    size_t i;
    const size_t length = strlen(channel);

    if(length < 2 || length > CHANNEL_NAME_MAX || channel[0] != '#')
        return false;
    for(i = 1; i < length; ++i) {
        const unsigned char c = (unsigned char)channel[i];
        if(c <= ' ' || c == ',' || c == 7)
            return false;
    }
    return true;
}

static int channel_page_find(const char *channel) {
    int i;

    for(i = 1; i < irc_page_count; ++i) {
        if(irc_name_equals(pages[i].name, channel))
            return i;
    }
    return -1;
}

static int channel_page_open(const char *channel) {
    irc_page_t *page;
    int existing = channel_page_find(channel);

    if(existing >= 0)
        return existing;
    if(irc_page_count >= MAX_PAGES)
        return -1;

    page = &pages[irc_page_count];
    memset(page, 0, sizeof(*page));
    snprintf(page->name, sizeof(page->name), "%s", channel);
    return irc_page_count++;
}

static void channel_page_close(int page_index) {
    if(page_index <= 0 || page_index >= irc_page_count)
        return;

    if(page_index + 1 < irc_page_count) {
        memmove(&pages[page_index], &pages[page_index + 1],
                sizeof(pages[0]) *
                (size_t)(irc_page_count - page_index - 1));
    }
    irc_page_count--;
    memset(&pages[irc_page_count], 0, sizeof(pages[irc_page_count]));

    if(active_page > page_index)
        active_page--;
    else if(active_page == page_index && active_page >= irc_page_count)
        active_page = irc_page_count - 1;
    screen_dirty = true;
}

static void switch_page(int direction) {
    if(irc_page_count <= 1)
        return;

    active_page = (active_page + direction + irc_page_count) % irc_page_count;
    screen_dirty = true;
}

static int maximum_scroll(const irc_page_t *page) {
    return page->history_count > VISIBLE_LINES ?
           page->history_count - VISIBLE_LINES : 0;
}

static void clamp_scroll(irc_page_t *page) {
    const int maximum = maximum_scroll(page);

    if(page->scroll_offset < 0)
        page->scroll_offset = 0;
    if(page->scroll_offset > maximum)
        page->scroll_offset = maximum;
}

static void page_history_push(int page_index, line_style_t style,
                              const char *text, size_t length) {
    irc_page_t *page;
    history_line_t *line;
    int line_index;

    if(page_index < 0 || page_index >= irc_page_count)
        page_index = 0;
    page = &pages[page_index];

    if(page->scroll_offset > 0)
        page->scroll_offset++;

    if(page->history_count < HISTORY_LINES) {
        line_index = (page->history_start + page->history_count) %
                     HISTORY_LINES;
        page->history_count++;
    }
    else {
        line_index = page->history_start;
        page->history_start = (page->history_start + 1) % HISTORY_LINES;
    }

    line = &page->history[line_index];
    if(length > UI_COLUMNS)
        length = UI_COLUMNS;
    memcpy(line->text, text, length);
    line->text[length] = '\0';
    line->style = style;
    clamp_scroll(page);
    if(page_index == active_page)
        screen_dirty = true;
}

static void page_history_add_wrapped(int page_index, line_style_t style,
                                     const char *text) {
    const char *cursor = text;

    if(!*cursor) {
        page_history_push(page_index, style, "", 0);
        return;
    }

    while(*cursor) {
        size_t remaining = strlen(cursor);
        size_t take = remaining;
        size_t i;

        if(take > UI_COLUMNS) {
            take = UI_COLUMNS;
            for(i = take; i > 0; --i) {
                if(cursor[i] == ' ') {
                    take = i;
                    break;
                }
            }
            if(take == 0)
                take = UI_COLUMNS;
        }

        while(take > 0 && cursor[take - 1] == ' ')
            take--;
        page_history_push(page_index, style, cursor, take);

        cursor += take;
        while(*cursor == ' ')
            cursor++;
    }
}

static void page_history_addf(int page_index, line_style_t style,
                              const char *format, ...) {
    char text[IRC_RECV_MAX + 128];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    page_history_add_wrapped(page_index, style, text);
}

static void history_addf(line_style_t style, const char *format, ...) {
    char text[IRC_RECV_MAX + 128];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    page_history_add_wrapped(active_page, style, text);
}

/* Convert incoming UTF-8 to the BIOS font's ISO-8859-1 and remove IRC
   presentation control codes. Unsupported Unicode is shown as '?'. */
static void remote_text_to_bios(char *output, size_t output_size,
                                const char *input) {
    size_t written = 0;
    const unsigned char *cursor = (const unsigned char *)input;

    if(output_size == 0)
        return;

    while(*cursor && written + 1 < output_size) {
        unsigned char c = *cursor++;

        if(c == 0x02 || c == 0x0f || c == 0x16 ||
           c == 0x1d || c == 0x1f) {
            continue;
        }

        if(c == 0x03) {
            int digits = 0;

            while(digits < 2 && *cursor >= '0' && *cursor <= '9') {
                cursor++;
                digits++;
            }
            if(*cursor == ',') {
                cursor++;
                digits = 0;
                while(digits < 2 && *cursor >= '0' && *cursor <= '9') {
                    cursor++;
                    digits++;
                }
            }
            continue;
        }

        if(c < 0x80) {
            if(c >= 0x20 || c == '\t')
                output[written++] = c == '\t' ? ' ' : (char)c;
        }
        else if(c == 0xc2 && *cursor >= 0x80 && *cursor <= 0xbf) {
            output[written++] = (char)*cursor++;
        }
        else if(c == 0xc3 && *cursor >= 0x80 && *cursor <= 0xbf) {
            output[written++] = (char)(*cursor++ + 0x40);
        }
        else {
            if((c & 0xe0) == 0xc0) {
                if((*cursor & 0xc0) == 0x80)
                    cursor++;
            }
            else if((c & 0xf0) == 0xe0) {
                int i;
                for(i = 0; i < 2 && (*cursor & 0xc0) == 0x80; ++i)
                    cursor++;
            }
            else if((c & 0xf8) == 0xf0) {
                int i;
                for(i = 0; i < 3 && (*cursor & 0xc0) == 0x80; ++i)
                    cursor++;
            }
            output[written++] = '?';
        }
    }

    output[written] = '\0';
}

static void page_history_add_remotef(int page_index, line_style_t style,
                                     const char *format, ...) {
    char utf8[IRC_RECV_MAX + 128];
    char bios[IRC_RECV_MAX + 128];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(utf8, sizeof(utf8), format, arguments);
    va_end(arguments);
    remote_text_to_bios(bios, sizeof(bios), utf8);
    page_history_add_wrapped(page_index, style, bios);
}

static void page_history_add_remote_line(int page_index, line_style_t style,
                                         const char *text) {
    char bios[IRC_RECV_MAX + 128];

    remote_text_to_bios(bios, sizeof(bios), text);
    page_history_push(page_index, style, bios, strlen(bios));
}

static void draw_rectangle(int x, int y, int width, int height,
                           uint16_t color) {
    int row;
    int column;

    for(row = y; row < y + height; ++row) {
        uint16_t *pixels = vram_s + row * SCREEN_WIDTH + x;
        for(column = 0; column < width; ++column)
            pixels[column] = color;
    }
}

static void draw_text(int x, int y, uint16_t color, const char *text) {
    bfont_draw_str_ex(vram_s + y * SCREEN_WIDTH + x, SCREEN_WIDTH,
                      color, 0, 16, false, text);
}

static uint16_t style_color(line_style_t style) {
    switch(style) {
        case STYLE_SELF:
            return rgb565(112, 224, 255);
        case STYLE_EVENT:
            return rgb565(148, 169, 190);
        case STYLE_NOTICE:
            return rgb565(255, 211, 96);
        case STYLE_ERROR:
            return rgb565(255, 112, 128);
        case STYLE_CHAT:
        default:
            return rgb565(228, 235, 242);
    }
}

static const char *connection_label(void) {
    switch(connection_state) {
        case CONN_CONNECTING:
            return "CONNECTING";
        case CONN_REGISTERING:
            return "REGISTERING";
        case CONN_JOINING:
            return "JOINING";
        case CONN_JOINED:
            return "ONLINE";
        case CONN_OFFLINE:
        default:
            return "OFFLINE";
    }
}

static void render_input_panel(void) {
    const uint16_t cyan = rgb565(74, 226, 231);
    const uint16_t white = rgb565(245, 248, 250);
    const uint16_t panel = rgb565(8, 22, 34);
    char input_display[UI_COLUMNS + 1];
    const char *shown_input = input_text;

    draw_rectangle(14, 392, 612, 40, panel);
    draw_rectangle(14, 392, 4, 40, cyan);

    if(input_length > UI_COLUMNS - 2)
        shown_input = input_text + input_length - (UI_COLUMNS - 2);
    snprintf(input_display, sizeof(input_display), "> %.*s",
             UI_COLUMNS - 2, shown_input);
    draw_text(28, 400, white, input_display);
}

static void render_screen(void) {
    const uint16_t header = rgb565(8, 48, 68);
    const uint16_t cyan = rgb565(74, 226, 231);
    const uint16_t white = rgb565(245, 248, 250);
    const uint16_t footer = rgb565(13, 35, 50);
    const irc_page_t *page = &pages[active_page];
    char page_label[UI_COLUMNS + 1];
    char status[UI_COLUMNS + 1];
    char footer_text[UI_COLUMNS + 1];
    int first;
    int visible;
    int i;

    if(!screen_dirty && !input_dirty)
        return;

    if(!screen_dirty) {
        render_input_panel();
        input_dirty = false;
        return;
    }

    vid_clear(3, 12, 20);

    draw_rectangle(0, 0, SCREEN_WIDTH, 62, header);
    draw_rectangle(0, 62, SCREEN_WIDTH, 2, cyan);
    draw_rectangle(0, 440, SCREEN_WIDTH, 40, footer);

    draw_text(24, 8, white, APP_NAME);
    if(active_page == 0) {
        snprintf(page_label, sizeof(page_label), "%s  server  %d/%d",
                 IRC_SERVER, active_page + 1, irc_page_count);
    }
    else {
        snprintf(page_label, sizeof(page_label), "%s  %s  %d/%d",
                 IRC_SERVER, page->name, active_page + 1, irc_page_count);
    }
    draw_text(24, 34, cyan, page_label);

    snprintf(status, sizeof(status), "%s  nick:%s", connection_label(),
             nickname[0] ? nickname : "pending");
    if(strlen(status) > 24)
        status[24] = '\0';
    draw_text(340, 8, connection_state == CONN_JOINED ?
              rgb565(116, 240, 153) : rgb565(255, 211, 96), status);

    clamp_scroll(&pages[active_page]);
    visible = page->history_count < VISIBLE_LINES ?
              page->history_count : VISIBLE_LINES;
    first = page->history_count - visible - page->scroll_offset;
    if(first < 0)
        first = 0;

    for(i = 0; i < visible; ++i) {
        const int line_index = (page->history_start + first + i) %
                               HISTORY_LINES;
        const history_line_t *line = &page->history[line_index];
        draw_text(28, 72 + i * BFONT_HEIGHT,
                  style_color(line->style), line->text);
    }

    render_input_panel();

    if(page->scroll_offset > 0) {
        snprintf(footer_text, sizeof(footer_text),
                 "SCROLL +%d  L/R:pages  X+Y:latest",
                 page->scroll_offset);
    }
    else if(!keyboard) {
        snprintf(footer_text, sizeof(footer_text),
                 "No keyboard  A+B:reconnect  START:quit");
    }
    else {
        snprintf(footer_text, sizeof(footer_text),
                 "Enter:send  Left/Right:pages  Up/Down:scroll");
    }
    draw_text(28, 448, rgb565(156, 184, 202), footer_text);
    screen_dirty = false;
    input_dirty = false;
}

static void set_generated_nickname(void) {
    unsigned number;

    if(!nickname_rng_seeded) {
        uint64_t entropy = timer_us_gettime64() ^
                           ((uint64_t)rtc_unix_secs() << 17);

        if(net_default_dev) {
            int i;
            for(i = 0; i < 6; ++i)
                entropy = entropy * 33u ^ net_default_dev->mac_addr[i];
        }
        srand((unsigned)(entropy ^ (entropy >> 32)));
        nickname_rng_seeded = true;
    }

    number = ((unsigned)rand() + nickname_attempt * 137u) % 1000u;
    snprintf(nickname, sizeof(nickname), "DCIRC_%03u", number);
    screen_dirty = true;
}

static void irc_close(void) {
    int i;

    if(irc_socket >= 0) {
        close(irc_socket);
        irc_socket = -1;
    }
    connection_state = CONN_OFFLINE;
    receive_length = 0;
    receive_overflow = false;
    list_in_progress = false;
    for(i = 1; i < irc_page_count; ++i)
        pages[i].joined = false;
    screen_dirty = true;
}

static int send_all(const char *data, size_t length) {
    size_t sent_total = 0;

    while(sent_total < length) {
        ssize_t sent = send(irc_socket, data + sent_total,
                            length - sent_total, 0);

        if(sent < 0 && errno == EINTR)
            continue;
        if(sent <= 0)
            return -1;
        sent_total += (size_t)sent;
    }
    return 0;
}

static int irc_sendf(const char *format, ...) {
    char line[IRC_WIRE_MAX];
    va_list arguments;
    int length;

    if(irc_socket < 0)
        return -1;

    va_start(arguments, format);
    length = vsnprintf(line, sizeof(line) - 2, format, arguments);
    va_end(arguments);

    if(length < 0)
        return -1;
    if(length > (int)sizeof(line) - 3)
        length = (int)sizeof(line) - 3;

    line[length++] = '\r';
    line[length++] = '\n';
    line[length] = '\0';

    printf("IRC >> %.*s\n", length - 2, line);
    if(send_all(line, (size_t)length) < 0) {
        const int error = errno;
        page_history_addf(0, STYLE_ERROR, "Send failed: %s",
                          strerror(error));
        printf("Dreamcast IRC: send failed: %s\n", strerror(error));
        irc_close();
        return -1;
    }
    return 0;
}

static int request_channel_join(const char *channel, bool focus) {
    int page_index;

    if(!valid_channel_name(channel)) {
        history_addf(STYLE_ERROR, "Invalid channel: %s", channel);
        return -1;
    }

    page_index = channel_page_open(channel);
    if(page_index < 0) {
        history_addf(STYLE_ERROR, "Channel limit reached (%d).",
                     MAX_CHANNELS);
        return -1;
    }

    if(focus) {
        active_page = page_index;
        pages[page_index].scroll_offset = 0;
        screen_dirty = true;
    }
    if(pages[page_index].joined) {
        page_history_addf(page_index, STYLE_NOTICE,
                          "Already joined %s.", channel);
        return page_index;
    }
    if(irc_socket < 0) {
        page_history_addf(page_index, STYLE_NOTICE,
                          "%s queued until reconnect.", channel);
        return page_index;
    }

    page_history_addf(page_index, STYLE_EVENT, "Joining %s...", channel);
    if(irc_sendf("JOIN %s", channel) < 0)
        return -1;
    connection_state = CONN_JOINING;
    return page_index;
}

static bool network_has_address(void) {
    return net_default_dev &&
           (net_default_dev->ip_addr[0] || net_default_dev->ip_addr[1] ||
            net_default_dev->ip_addr[2] || net_default_dev->ip_addr[3]);
}

static int irc_connect(void) {
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    int resolver_error;
    int last_error = 0;

    if(irc_socket >= 0)
        irc_sendf("QUIT :Reconnecting");
    irc_close();
    active_page = 0;
    if(!network_has_address()) {
        page_history_addf(0, STYLE_ERROR,
                          "No IPv4 address. Enable the BBA and DHCP.");
        printf("Dreamcast IRC: no configured network device\n");
        return -1;
    }

    connection_state = CONN_CONNECTING;
    page_history_addf(0, STYLE_EVENT, "Resolving %s...", IRC_SERVER);
    render_screen();

    if(!net_default_dev->dns[0] && !net_default_dev->dns[1] &&
       !net_default_dev->dns[2] && !net_default_dev->dns[3]) {
        net_default_dev->dns[0] = 1;
        net_default_dev->dns[1] = 1;
        net_default_dev->dns[2] = 1;
        net_default_dev->dns[3] = 1;
        printf("Dreamcast IRC: DHCP supplied no DNS; using 1.1.1.1\n");
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    resolver_error = getaddrinfo(IRC_SERVER, IRC_PORT, &hints, &addresses);
    if(resolver_error != 0) {
        history_addf(STYLE_ERROR, "DNS lookup failed (error %d).",
                     resolver_error);
        printf("Dreamcast IRC: DNS lookup failed: %d\n", resolver_error);
        irc_close();
        return -1;
    }

    page_history_addf(0, STYLE_EVENT, "Connecting to %s:%s...",
                      IRC_SERVER, IRC_PORT);
    render_screen();

    for(address = addresses; address; address = address->ai_next) {
        int candidate = socket(address->ai_family, address->ai_socktype,
                               address->ai_protocol);

        if(candidate < 0) {
            last_error = errno;
            continue;
        }

        if(connect(candidate, address->ai_addr, address->ai_addrlen) == 0) {
            int enabled = 1;
            irc_socket = candidate;
            setsockopt(irc_socket, IPPROTO_TCP, TCP_NODELAY,
                       &enabled, sizeof(enabled));
            break;
        }

        last_error = errno;
        close(candidate);
    }
    freeaddrinfo(addresses);

    if(irc_socket < 0) {
        page_history_addf(0, STYLE_ERROR, "Connection failed: %s",
                          strerror(last_error));
        printf("Dreamcast IRC: connection failed: %s\n",
               strerror(last_error));
        irc_close();
        return -1;
    }

    receive_length = 0;
    receive_overflow = false;
    connection_state = CONN_REGISTERING;
    page_history_addf(0, STYLE_EVENT, "Connected. Registering as %s...",
                      nickname);
    printf("Dreamcast IRC: connected to %s:%s as %s\n",
           IRC_SERVER, IRC_PORT, nickname);

    if(irc_sendf("NICK %s", nickname) < 0 ||
       irc_sendf("USER dreamcast 0 * :Dreamcast IRC Client") < 0) {
        irc_close();
        return -1;
    }

    screen_dirty = true;
    return 0;
}

static void prefix_nickname(const char *prefix, char *output,
                            size_t output_size) {
    const char *end;
    size_t length;

    if(!prefix || !*prefix) {
        snprintf(output, output_size, "server");
        return;
    }

    end = strchr(prefix, '!');
    length = end ? (size_t)(end - prefix) : strlen(prefix);
    if(length >= output_size)
        length = output_size - 1;
    memcpy(output, prefix, length);
    output[length] = '\0';
}

static void handle_irc_line(const char *wire_line) {
    char line[IRC_RECV_MAX];
    char *parameters[16];
    char *cursor;
    char *prefix = NULL;
    char *command;
    char sender[64];
    int parameter_count = 0;

    snprintf(line, sizeof(line), "%s", wire_line);
    if(!list_in_progress || !strstr(wire_line, " 322 ") ||
       list_entries_received < LIST_DISPLAY_MAX) {
        printf("IRC << %s\n", line);
    }
    cursor = line;

    if(*cursor == ':') {
        prefix = ++cursor;
        cursor = strchr(cursor, ' ');
        if(!cursor)
            return;
        *cursor++ = '\0';
    }

    while(*cursor == ' ')
        cursor++;
    command = cursor;
    cursor = strchr(cursor, ' ');
    if(cursor)
        *cursor++ = '\0';

    while(cursor && *cursor && parameter_count < 16) {
        while(*cursor == ' ')
            cursor++;
        if(!*cursor)
            break;
        if(*cursor == ':') {
            parameters[parameter_count++] = cursor + 1;
            break;
        }
        parameters[parameter_count++] = cursor;
        cursor = strchr(cursor, ' ');
        if(cursor)
            *cursor++ = '\0';
    }

    prefix_nickname(prefix, sender, sizeof(sender));

    if(strcmp(command, "PING") == 0) {
        if(parameter_count > 0)
            irc_sendf("PONG :%s", parameters[0]);
        return;
    }

    if(strcmp(command, "001") == 0) {
        int i;

        connection_state = CONN_JOINING;
        page_history_addf(0, STYLE_EVENT, "Registered with %s.", IRC_SERVER);
        if(irc_page_count == 1) {
            int page_index = channel_page_open(IRC_CHANNEL);
            if(page_index >= 0)
                active_page = page_index;
        }
        for(i = 1; i < irc_page_count; ++i) {
            pages[i].joined = false;
            page_history_addf(i, STYLE_EVENT, "Joining %s...", pages[i].name);
            if(irc_sendf("JOIN %s", pages[i].name) < 0)
                break;
        }
        if(irc_page_count == 1)
            connection_state = CONN_JOINED;
        screen_dirty = true;
        return;
    }

    if(strcmp(command, "433") == 0) {
        nickname_attempt++;
        set_generated_nickname();
        page_history_addf(0, STYLE_NOTICE,
                          "Nickname in use; trying %s.", nickname);
        irc_sendf("NICK %s", nickname);
        return;
    }

    if(strcmp(command, "PRIVMSG") == 0 && parameter_count >= 2) {
        const char *message = parameters[1];
        size_t length = strlen(message);
        int page_index = channel_page_find(parameters[0]);

        if(page_index < 0)
            page_index = 0;

        if(length == 9 && memcmp(message, "\001VERSION\001", 9) == 0 &&
           irc_name_equals(parameters[0], nickname)) {
            irc_sendf("NOTICE %s :\001VERSION " APP_NAME " " APP_VERSION
                      " | Sega Dreamcast | Hitachi SH-4 200 MHz | "
                      "16 MB RAM, 8 MB VRAM, 2 MB ARAM | KallistiOS %s\001",
                      sender, KOS_VERSION_STRING);
            page_history_add_remotef(0, STYLE_NOTICE,
                                     "CTCP VERSION requested by %s", sender);
        }
        else if(length > 9 && message[0] == '\001' &&
           strncmp(message + 1, "ACTION ", 7) == 0 &&
           message[length - 1] == '\001') {
            char action[IRC_RECV_MAX];
            size_t action_length = length - 9;
            if(action_length >= sizeof(action))
                action_length = sizeof(action) - 1;
            memcpy(action, message + 8, action_length);
            action[action_length] = '\0';
            page_history_add_remotef(page_index, STYLE_CHAT,
                                     "* %s %s", sender, action);
        }
        else {
            if(page_index == 0 &&
               irc_name_equals(parameters[0], nickname)) {
                page_history_add_remotef(0, STYLE_CHAT,
                                         "[PM] <%s> %s", sender, message);
            }
            else {
                page_history_add_remotef(page_index, STYLE_CHAT,
                                         "<%s> %s", sender, message);
            }
        }
        return;
    }

    if(strcmp(command, "NOTICE") == 0 && parameter_count >= 2) {
        int page_index = channel_page_find(parameters[0]);
        if(page_index < 0)
            page_index = 0;
        page_history_add_remotef(page_index, STYLE_NOTICE,
                                 "-%s- %s", sender, parameters[1]);
        return;
    }

    if(strcmp(command, "JOIN") == 0 && parameter_count >= 1) {
        int page_index = channel_page_find(parameters[0]);

        if(irc_name_equals(sender, nickname)) {
            if(page_index < 0)
                page_index = channel_page_open(parameters[0]);
            if(page_index < 0) {
                page_history_addf(0, STYLE_ERROR,
                                  "Cannot open %s: channel limit reached.",
                                  parameters[0]);
                irc_sendf("PART %s :DCIRC channel limit", parameters[0]);
                return;
            }
            pages[page_index].joined = true;
            connection_state = CONN_JOINED;
            page_history_addf(page_index, STYLE_EVENT,
                              "Joined %s. Welcome!", parameters[0]);
        }
        else if(page_index >= 0) {
            page_history_add_remotef(page_index, STYLE_EVENT,
                                     "--> %s joined", sender);
        }
        return;
    }

    if(strcmp(command, "PART") == 0 && parameter_count >= 1) {
        int page_index = channel_page_find(parameters[0]);

        if(page_index < 0)
            return;
        if(irc_name_equals(sender, nickname)) {
            page_history_addf(0, STYLE_EVENT, "Left %s%s%s", parameters[0],
                              parameter_count >= 2 ? ": " : "",
                              parameter_count >= 2 ? parameters[1] : "");
            channel_page_close(page_index);
            connection_state = CONN_JOINED;
        }
        else {
            page_history_add_remotef(page_index, STYLE_EVENT,
                                     "<-- %s left%s%s", sender,
                                     parameter_count >= 2 ? ": " : "",
                                     parameter_count >= 2 ? parameters[1] : "");
        }
        return;
    }

    if(strcmp(command, "QUIT") == 0) {
        page_history_add_remotef(0, STYLE_EVENT, "<-- %s quit%s%s", sender,
                                 parameter_count ? ": " : "",
                                 parameter_count ? parameters[0] : "");
        return;
    }

    if(strcmp(command, "KICK") == 0 && parameter_count >= 2) {
        int page_index = channel_page_find(parameters[0]);
        if(page_index < 0)
            page_index = 0;
        page_history_add_remotef(page_index, STYLE_NOTICE,
                                 "%s kicked %s%s%s", sender, parameters[1],
                                 parameter_count >= 3 ? ": " : "",
                                 parameter_count >= 3 ? parameters[2] : "");
        if(irc_name_equals(parameters[1], nickname) && page_index > 0) {
            page_history_addf(0, STYLE_NOTICE, "Kicked from %s by %s.",
                              parameters[0], sender);
            channel_page_close(page_index);
            connection_state = CONN_JOINED;
        }
        return;
    }

    if(strcmp(command, "NICK") == 0 && parameter_count >= 1) {
        page_history_add_remotef(0, STYLE_EVENT, "%s is now known as %s",
                                 sender, parameters[0]);
        if(irc_name_equals(sender, nickname)) {
            snprintf(nickname, sizeof(nickname), "%s", parameters[0]);
            screen_dirty = true;
        }
        return;
    }

    if(strcmp(command, "332") == 0 && parameter_count >= 3) {
        int page_index = channel_page_find(parameters[1]);
        if(page_index < 0)
            page_index = 0;
        page_history_add_remotef(page_index, STYLE_NOTICE,
                                 "Topic: %s", parameters[2]);
        return;
    }

    if(strcmp(command, "353") == 0 && parameter_count >= 4) {
        int page_index = channel_page_find(parameters[2]);
        if(page_index < 0)
            page_index = 0;
        page_history_add_remotef(page_index, STYLE_EVENT,
                                 "Names: %s", parameters[3]);
        return;
    }

    if(strcmp(command, "321") == 0) {
        list_entries_received = 0;
        list_entries_displayed = 0;
        list_in_progress = true;
        page_history_addf(0, STYLE_NOTICE, "Channel list:");
        return;
    }

    if(strcmp(command, "322") == 0 && parameter_count >= 3) {
        list_entries_received++;
        if(list_entries_displayed < LIST_DISPLAY_MAX) {
            char summary[IRC_RECV_MAX + 128];
            snprintf(summary, sizeof(summary), "%s [%s] %s", parameters[1],
                     parameters[2], parameter_count >= 4 ? parameters[3] : "");
            page_history_add_remote_line(0, STYLE_CHAT, summary);
            list_entries_displayed++;
        }
        return;
    }

    if(strcmp(command, "323") == 0) {
        page_history_addf(0, STYLE_NOTICE,
                          "End of LIST: %u received, %u shown.",
                          list_entries_received, list_entries_displayed);
        list_in_progress = false;
        return;
    }

    if(strcmp(command, "ERROR") == 0) {
        page_history_add_remotef(0, STYLE_ERROR, "Server error: %s",
                                 parameter_count ? parameters[0] :
                                 "disconnected");
        irc_close();
        return;
    }

    if(command[0] >= '4' && command[0] <= '5' &&
       parameter_count > 0) {
        page_history_add_remotef(0, STYLE_ERROR, "%s: %s", command,
                                 parameters[parameter_count - 1]);
        return;
    }

    if(strlen(command) == 3 && command[0] >= '0' && command[0] <= '9' &&
       command[1] >= '0' && command[1] <= '9' &&
       command[2] >= '0' && command[2] <= '9' && parameter_count > 0) {
        page_history_add_remotef(0, STYLE_EVENT, "%s: %s", command,
                                 parameters[parameter_count - 1]);
    }
}

static void consume_received_bytes(const char *data, size_t length) {
    size_t i;

    for(i = 0; i < length; ++i) {
        const char c = data[i];

        if(c == '\n') {
            if(receive_overflow) {
                page_history_addf(0, STYLE_ERROR,
                                  "Server sent an overlong IRC line.");
            }
            else {
                if(receive_length > 0 &&
                   receive_line[receive_length - 1] == '\r') {
                    receive_length--;
                }
                receive_line[receive_length] = '\0';
                if(receive_length > 0)
                    handle_irc_line(receive_line);
            }
            receive_length = 0;
            receive_overflow = false;
        }
        else if(!receive_overflow) {
            if(receive_length + 1 < sizeof(receive_line))
                receive_line[receive_length++] = c;
            else
                receive_overflow = true;
        }
    }
}

static void poll_network(void) {
    char buffer[512];
    int reads;

    if(irc_socket < 0)
        return;

    for(reads = 0; reads < 16; ++reads) {
        ssize_t received = recv(irc_socket, buffer, sizeof(buffer), MSG_DONTWAIT);

        if(received > 0) {
            consume_received_bytes(buffer, (size_t)received);
            continue;
        }
        if(received == 0) {
            page_history_addf(0, STYLE_ERROR,
                              "Connection closed by the server.");
            printf("Dreamcast IRC: server closed the connection\n");
            irc_close();
            return;
        }
        if(errno == EINTR)
            continue;
        if(errno != EAGAIN && errno != EWOULDBLOCK) {
            const int error = errno;
            page_history_addf(0, STYLE_ERROR, "Receive failed: %s",
                              strerror(error));
            printf("Dreamcast IRC: receive failed: %s\n", strerror(error));
            irc_close();
        }
        return;
    }
}

static bool valid_nickname(const char *candidate) {
    size_t i;
    size_t length = strlen(candidate);

    if(length == 0 || length >= sizeof(nickname))
        return false;
    if(!((candidate[0] >= 'A' && candidate[0] <= 'Z') ||
         (candidate[0] >= 'a' && candidate[0] <= 'z') ||
         candidate[0] == '_')) {
        return false;
    }
    for(i = 1; i < length; ++i) {
        const char c = candidate[i];
        if(!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
             (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static void latin1_to_utf8(char *output, size_t output_size,
                           const char *input) {
    size_t written = 0;
    const unsigned char *cursor = (const unsigned char *)input;

    while(*cursor && written + 1 < output_size) {
        unsigned char c = *cursor++;
        if(c < 0x80) {
            output[written++] = (char)c;
        }
        else if(written + 2 < output_size) {
            output[written++] = (char)(0xc0 | (c >> 6));
            output[written++] = (char)(0x80 | (c & 0x3f));
        }
        else {
            break;
        }
    }
    output[written] = '\0';
}

static void command_join_channels(char *argument) {
    char *cursor = argument;

    if(!cursor || !*cursor) {
        history_addf(STYLE_ERROR, "Usage: /join #CHANNEL[,#CHANNEL]");
        return;
    }

    while(cursor && *cursor) {
        char *comma = strchr(cursor, ',');

        if(comma)
            *comma = '\0';
        request_channel_join(cursor, true);
        cursor = comma ? comma + 1 : NULL;
    }
}

static void command_part_channel(char *argument) {
    char *channel = NULL;
    char *reason = NULL;
    int page_index;
    char utf8[INPUT_MAX * 2 + 1];

    if(argument && *argument && argument[0] == '#') {
        char *space = strchr(argument, ' ');
        channel = argument;
        if(space) {
            *space++ = '\0';
            while(*space == ' ')
                space++;
            reason = space;
        }
    }
    else if(active_page > 0) {
        channel = pages[active_page].name;
        reason = argument;
    }

    if(!channel || !valid_channel_name(channel)) {
        history_addf(STYLE_ERROR, "Usage: /part [#CHANNEL] [MESSAGE]");
        return;
    }

    page_index = channel_page_find(channel);
    if(page_index < 0) {
        history_addf(STYLE_ERROR, "Not joined to %s.", channel);
        return;
    }

    if(irc_socket < 0 || !pages[page_index].joined) {
        page_history_addf(0, STYLE_EVENT, "Closed %s.", channel);
        channel_page_close(page_index);
        return;
    }

    latin1_to_utf8(utf8, sizeof(utf8),
                   reason && *reason ? reason : "Leaving");
    if(irc_sendf("PART %s :%s", channel, utf8) < 0)
        channel_page_close(page_index);
}

static void submit_input(void) {
    char utf8[INPUT_MAX * 2 + 1];

    if(input_length == 0)
        return;

    if(input_text[0] == '/') {
        char *argument = strchr(input_text, ' ');

        if(argument) {
            *argument++ = '\0';
            while(*argument == ' ')
                argument++;
        }

        if(strcmp(input_text, "/help") == 0) {
            history_addf(STYLE_NOTICE,
                         "Commands: /join /part /list /me /nick /clear "
                         "/reconnect /quit");
        }
        else if(strcmp(input_text, "/clear") == 0) {
            pages[active_page].history_start = 0;
            pages[active_page].history_count = 0;
            pages[active_page].scroll_offset = 0;
            screen_dirty = true;
        }
        else if(strcmp(input_text, "/reconnect") == 0) {
            irc_connect();
        }
        else if(strcmp(input_text, "/join") == 0) {
            command_join_channels(argument);
        }
        else if(strcmp(input_text, "/part") == 0) {
            command_part_channel(argument);
        }
        else if(strcmp(input_text, "/list") == 0) {
            active_page = 0;
            pages[0].scroll_offset = 0;
            screen_dirty = true;
            if(irc_socket >= 0) {
                list_entries_received = 0;
                list_entries_displayed = 0;
                list_in_progress = true;
                page_history_addf(0, STYLE_NOTICE,
                                  "Requesting channel list%s%s...",
                                  argument && *argument ? ": " : "",
                                  argument && *argument ? argument : "");
                if(argument && *argument)
                    irc_sendf("LIST %s", argument);
                else
                    irc_sendf("LIST");
            }
            else {
                page_history_addf(0, STYLE_ERROR,
                                  "Connect before requesting /list.");
            }
        }
        else if(strcmp(input_text, "/nick") == 0) {
            if(argument && valid_nickname(argument)) {
                irc_sendf("NICK %s", argument);
            }
            else {
                history_addf(STYLE_ERROR,
                             "Usage: /nick NAME (letters, digits, _ or -)");
            }
        }
        else if(strcmp(input_text, "/me") == 0) {
            if(argument && *argument && active_page > 0 &&
               pages[active_page].joined && irc_socket >= 0) {
                latin1_to_utf8(utf8, sizeof(utf8), argument);
                if(irc_sendf("PRIVMSG %s :\001ACTION %s\001",
                             pages[active_page].name, utf8) == 0) {
                    history_addf(STYLE_SELF, "* %s %s", nickname, argument);
                }
            }
            else {
                history_addf(STYLE_ERROR, "Usage: /me ACTION (while online)");
            }
        }
        else if(strcmp(input_text, "/quit") == 0) {
            if(irc_socket >= 0) {
                latin1_to_utf8(utf8, sizeof(utf8),
                               argument && *argument ? argument : "Leaving");
                irc_sendf("QUIT :%s", utf8);
                irc_close();
            }
            running = false;
        }
        else {
            history_addf(STYLE_ERROR, "Unknown command. Type /help.");
        }
    }
    else if(active_page > 0 && pages[active_page].joined &&
            irc_socket >= 0) {
        latin1_to_utf8(utf8, sizeof(utf8), input_text);
        if(irc_sendf("PRIVMSG %s :%s", pages[active_page].name, utf8) == 0)
            history_addf(STYLE_SELF, "<%s> %s", nickname, input_text);
    }
    else {
        history_addf(STYLE_ERROR,
                     active_page == 0 ?
                     "Server page: use /join #CHANNEL to chat." :
                     "Channel is not joined; use /join or reconnect.");
    }

    input_length = 0;
    input_text[0] = '\0';
    screen_dirty = true;
}

static void poll_keyboard(void) {
    int event;
    kbd_state_t *keyboard_state;

    if(!keyboard || !keyboard->valid) {
        maple_device_t *found = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
        if(found != keyboard) {
            keyboard = found;
            screen_dirty = true;
            if(keyboard)
                history_addf(STYLE_EVENT, "Dreamcast keyboard connected.");
        }
    }
    if(!keyboard)
        return;

    keyboard_state = kbd_get_state(keyboard);
    if(!keyboard_state)
        return;

    /* Read packed raw events so non-printable HID key codes cannot be
       mistaken for same-valued ASCII. For example, Up is 0x52 ('R') and
       Down is 0x51 ('Q'). */
    while((event = kbd_queue_pop(keyboard, false)) != KBD_QUEUE_END) {
        const kbd_key_t key = (kbd_key_t)(event & 0xff);
        kbd_mods_t modifiers;
        kbd_leds_t leds;
        unsigned char character;

        modifiers.raw = (uint8_t)((event >> 8) & 0xff);
        leds.raw = (uint8_t)((event >> 16) & 0xff);

        if(key == KBD_KEY_ENTER || key == KBD_KEY_PAD_ENTER) {
            submit_input();
        }
        else if(key == KBD_KEY_BACKSPACE) {
            if(input_length > 0) {
                input_text[--input_length] = '\0';
                input_dirty = true;
            }
        }
        else if(key == KBD_KEY_PGUP || key == KBD_KEY_UP) {
            pages[active_page].scroll_offset += VISIBLE_LINES / 2;
            clamp_scroll(&pages[active_page]);
            screen_dirty = true;
        }
        else if(key == KBD_KEY_PGDOWN || key == KBD_KEY_DOWN) {
            pages[active_page].scroll_offset -= VISIBLE_LINES / 2;
            clamp_scroll(&pages[active_page]);
            screen_dirty = true;
        }
        else if(key == KBD_KEY_LEFT) {
            switch_page(-1);
        }
        else if(key == KBD_KEY_RIGHT) {
            switch_page(1);
        }
        else {
            character = (unsigned char)kbd_key_to_ascii(
                key, keyboard_state->region, modifiers, leds);
            if(character < 32 || input_length >= INPUT_MAX)
                continue;
            input_text[input_length++] = (char)character;
            input_text[input_length] = '\0';
            input_dirty = true;
        }
    }
}

static void poll_controller(void) {
    maple_device_t *controller = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    uint32_t buttons = 0;
    uint32_t pressed;
    const uint32_t reconnect_chord = CONT_A | CONT_B;
    const uint32_t latest_chord = CONT_X | CONT_Y;

    if(controller && controller->valid) {
        cont_state_t *state = (cont_state_t *)maple_dev_status(controller);
        if(state)
            buttons = state->buttons;
    }

    pressed = buttons & ~previous_buttons;

    if(pressed & CONT_START)
        running = false;
    if((buttons & reconnect_chord) == reconnect_chord &&
       (previous_buttons & reconnect_chord) != reconnect_chord) {
        irc_connect();
    }
    if((buttons & latest_chord) == latest_chord &&
       (previous_buttons & latest_chord) != latest_chord) {
        pages[active_page].scroll_offset = 0;
        screen_dirty = true;
    }
    if(pressed & CONT_DPAD_UP) {
        pages[active_page].scroll_offset += VISIBLE_LINES / 2;
        clamp_scroll(&pages[active_page]);
        screen_dirty = true;
    }
    if(pressed & CONT_DPAD_DOWN) {
        pages[active_page].scroll_offset -= VISIBLE_LINES / 2;
        clamp_scroll(&pages[active_page]);
        screen_dirty = true;
    }
    if(pressed & CONT_DPAD_LEFT)
        switch_page(-1);
    if(pressed & CONT_DPAD_RIGHT)
        switch_page(1);
    previous_buttons = buttons;
}

static void print_network_details(void) {
    if(net_default_dev) {
        const uint8_t *ip = net_default_dev->ip_addr;
        const uint8_t *gateway = net_default_dev->gateway;
        const uint8_t *dns = net_default_dev->dns;

        printf("Dreamcast IRC netif: %s\n", net_default_dev->name);
        printf("Dreamcast IRC IPv4: %u.%u.%u.%u\n",
               ip[0], ip[1], ip[2], ip[3]);
        printf("Dreamcast IRC gateway: %u.%u.%u.%u\n",
               gateway[0], gateway[1], gateway[2], gateway[3]);
        printf("Dreamcast IRC DNS: %u.%u.%u.%u\n",
               dns[0], dns[1], dns[2], dns[3]);
    }
    else {
        printf("Dreamcast IRC: net_default_dev is NULL\n");
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* A single framebuffer lets ordinary typing redraw only the input panel.
       KOS's DM_MULTIBUFFER mode allocates 13 full 640x480 buffers here, which
       requires rebuilding the whole UI to keep every buffer synchronized. */
    vid_set_mode(DM_640x480, PM_RGB565);
    vid_border_color(3, 12, 20);
    bfont_set_encoding(BFONT_CODE_ISO8859_1);
    keyboard = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    snprintf(pages[0].name, sizeof(pages[0].name), "Server");

    print_network_details();
    set_generated_nickname();
    history_addf(STYLE_EVENT, APP_NAME " starting.");
    history_addf(STYLE_EVENT, "Server: %s:%s", IRC_SERVER, IRC_PORT);
    history_addf(STYLE_EVENT, "Default channel: %s", IRC_CHANNEL);
    if(!keyboard)
        history_addf(STYLE_NOTICE, "Connect a Dreamcast keyboard to type.");
    history_addf(STYLE_NOTICE, "Type /help for commands. START quits.");
    render_screen();

    irc_connect();

    while(running) {
        poll_network();
        poll_keyboard();
        poll_controller();
        render_screen();
        thd_sleep(2);
    }

    if(irc_socket >= 0)
        irc_sendf("QUIT :Dreamcast powering down");
    irc_close();
    printf("Dreamcast IRC: clean shutdown\n");
    return 0;
}
