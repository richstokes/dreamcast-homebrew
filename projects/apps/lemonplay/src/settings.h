/* Settings kept on the first VMU: the picture mode and the last server. */
#ifndef LEMONPLAY_SETTINGS_H
#define LEMONPLAY_SETTINGS_H

/* How video fills the screen. Start cycles through them. */
enum {
    PICTURE_FIT,    /* whole picture, black bars as needed (4:3 TVs, and 16:9 TVs showing 4:3) */
    PICTURE_ZOOM,   /* widescreen cropped to fill a 4:3 screen */
    PICTURE_WIDE,   /* drawn for a 16:9 TV that stretches the Dreamcast's picture */
    PICTURE_MODES
};

typedef struct {
    int picture;
    char server[96];    /* description.xml URL of the last server opened */
} settings_t;

extern settings_t settings;

const char *picture_name(int mode);
const char *picture_help(int mode);

void settings_load(void);
/* Marks the settings changed; settings_poll writes them a moment later. */
void settings_changed(void);
/* Call from screens where a VMU write can't disturb playback. */
void settings_poll(void);

#endif
