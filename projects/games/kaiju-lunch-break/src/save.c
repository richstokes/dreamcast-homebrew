/* The best lunch so far (save.h). */
#include "save.h"
#include "audio.h"
#include "engine/cardpick.h"
#include <stddef.h>

#define SAVE_MAGIC 0x4b4c4231u   /* "KLB1" */
Save save = {.magic = SAVE_MAGIC};

/* Test builds leave the cards alone, bar the card test itself. */
#if !defined(KL_AUTOTEST) || defined(KL_AT_VMUTEST)
#define SAVE_FILE "KAIJULNCH"
#define SAVE_BLOCKS vmusave_blocks(0, sizeof(Save))

enum { CARD_NONE, CARD_OFF, CARD_LOOKING, CARD_ASK, CARD_SET };
static int card_state = CARD_NONE, card_port = -1, card_unit = -1, ask_kind;
static vmusave_slot_t slots[VMUSAVE_SLOTS];   /* the last look; their files are kept only while asking */
static int nslots;
static char notes[VMUSAVE_SLOTS][16];
static bool stands;        /* what is in memory is the save (loaded, or earned with no card in) */
static bool unsaved, looked;
static uint32_t looked_mask;
static kthread_t *look_thd;
static volatile bool look_done;

static bool parse(const vmusave_slot_t *slot, Save *out) {
    const uint8_t *data;
    int n = vmusave_slot_payload(slot, &data);
    /* (a save from before the options word is taken with none set) */
    if(n != (int)sizeof(Save) && n != (int)offsetof(Save, options)) return false;
    memset(out, 0, sizeof(Save));
    memcpy(out, data, (size_t)n);
    return out->magic == SAVE_MAGIC;
}

static void write_card(void) {
    maple_device_t *card = maple_enum_dev(card_port, card_unit);
    vmu_pkg_t pkg;
    vmusave_header(&pkg, "KAIJULUNCH", "Kaiju Lunch", "Kaiju Lunch Break best lunch");
    unsaved = !card || vmusave_write(card, SAVE_FILE, &pkg, &save, sizeof(save)) < 0;
    if(unsaved) printf("KL save: could not write to VMU %c%d\n", 'A' + card_port, card_unit);
}

static void use_card(int i) {
    Save s;
    card_port = slots[i].port;
    card_unit = slots[i].unit;
    card_state = CARD_SET;
    if(!stands && slots[i].save && parse(&slots[i], &s)) {
        save = s;
        printf("KL save: loaded from VMU %c%d\n", 'A' + card_port, card_unit);
    }
    stands = true;
    vmusave_survey_free(slots, nslots);
    /* A record set with no card to go to is written now. */
    if(unsaved) write_card();
}

static void *look_main(void *arg) {
    (void)arg;
    nslots = vmusave_survey(SAVE_FILE, slots, VMUSAVE_SLOTS);
    look_done = true;
    return NULL;
}

/* What the look found: settled, or made a question of. */
static void look_landed(void) {
    int home = -1, readable = 0, pick = 0;
    looked = true;
    card_state = CARD_NONE;
    for(int i = 0; i < nslots; i++) {
        Save s;
        notes[i][0] = '\0';
        if(slots[i].free_blocks >= 0) readable++;
        if(slots[i].save && !parse(&slots[i], &s)) slots[i].save = false;
        else if(slots[i].save) snprintf(notes[i], sizeof(notes[i]), "$%lluB", (unsigned long long)(s.best_total / 1000000u));
        if(slots[i].port == card_port && slots[i].unit == card_unit) home = i;
    }
    if(stands) {
        /* The record in memory only wants somewhere to go: its own card
           while that is in and has room, otherwise where the player says,
           as it would be written over any save another card holds. */
        if(home >= 0 && vmusave_fits(&slots[home], SAVE_BLOCKS)) use_card(home);
        else if(readable) {
            card_state = CARD_ASK;
            ask_kind = CARDPICK_WHERE;
        }
        else vmusave_survey_free(slots, nslots);
        return;
    }
    switch(vmusave_decide(slots, nslots, SAVE_BLOCKS, &pick)) {
        case VMUSAVE_USE: use_card(pick); break;
        case VMUSAVE_ASK_WHICH:
            card_state = CARD_ASK;
            ask_kind = CARDPICK_WHICH;
            break;
        case VMUSAVE_ASK_ROOM:
            card_state = CARD_ASK;
            ask_kind = CARDPICK_ROOM;
            break;
        default: vmusave_survey_free(slots, nslots); break;
    }
}

void save_cards(void) {
    if(card_state == CARD_LOOKING || card_state == CARD_ASK) return;
    uint32_t plugged = vmusave_plugged();
    if(card_state == CARD_SET && !unsaved && maple_enum_dev(card_port, card_unit)) return;
    /* Declined, or no card that would do: nothing to learn until one
       comes or goes. */
    if((card_state == CARD_OFF || card_state == CARD_NONE) && looked && plugged == looked_mask) return;
    vmusave_survey_free(slots, nslots);
    nslots = 0;
    looked_mask = plugged;
    look_done = false;
    card_state = CARD_LOOKING;
    /* On a thread: two cards are over half a second of block reads. */
    look_thd = thd_create(false, look_main, NULL);
    if(!look_thd) look_main(NULL);
}

static bool looking(void) {
    if(card_state != CARD_LOOKING) return false;
    if(!look_done) return true;
    if(look_thd) thd_join(look_thd, NULL);
    look_thd = NULL;
    look_landed();
    return false;
}

bool save_hold(void) {
    if(looking()) return true;
    if(card_state != CARD_ASK) return false;
    if(!cardpick_up()) {
        static const char *note_ptr[VMUSAVE_SLOTS];
        for(int i = 0; i < nslots; i++) note_ptr[i] = notes[i];
        cardpick_open(ask_kind, slots, note_ptr, nslots, SAVE_BLOCKS);
        return true;
    }
    int r = cardpick_update();
    if(r == CARDPICK_MOVED) audio_sfx(KL_SFX_MOVE, 150);
    else if(r != CARDPICK_WAIT) {
        audio_sfx(KL_SFX_OK, 170);
        if(r >= 0 && r < nslots) use_card(r);
        else {
            card_state = CARD_OFF;
            vmusave_survey_free(slots, nslots);
            printf("KL save: playing without saving\n");
        }
    }
    return true;
}

void save_draw(void) {
    if(cardpick_up()) cardpick_draw();
}

static void keep(void) {
    stands = true;
    if(card_state == CARD_SET) write_card();
    else unsaved = true;
}

#ifdef KL_AT_VMUTEST
bool save_wait(void) {
    while(looking()) thd_sleep(10);
    return card_state != CARD_ASK;
}

void save_forget(void) {
    vmusave_survey_free(slots, nslots);
    memset(&save, 0, sizeof(save));
    save.magic = SAVE_MAGIC;
    stands = unsaved = looked = false;
    card_state = CARD_NONE;
    card_port = card_unit = -1;
    nslots = 0;
}
#endif

#else
void save_cards(void) {}
bool save_hold(void) { return false; }
void save_draw(void) {}
static void keep(void) {}
#endif

void save_changed(void) {
    keep();
}

bool save_submit(uint64_t total, int mult, int grade) {
    bool record = total > save.best_total;
    save.lunches++;
    if(record) {
        save.best_total = total;
        save.grade = (uint32_t)grade;
    }
    if((uint32_t)mult > save.best_mult) save.best_mult = (uint32_t)mult;
    keep();
    return record;
}
