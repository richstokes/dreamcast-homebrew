#include "client.h"
#include <kos.h>
#include <dc/biosfont.h>
#include <dc/maple/controller.h>
#include <dc/maple/keyboard.h>
#include <dc/vmufs.h>
#include <dc/video.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);
#define MAX_FILES 1600
#define INK 0x1927
#define PAPER 0xef7d
#define ORANGE 0xa9a1
#define BLUE 0x1a75

typedef enum { STARTUP, AUTO_LOGIN, LOGIN, HOME, FILES, DETAILS, CONFLICT, SUCCESS, PUBLIC_SEARCH, DOWNLOADS, DESTINATION, INSTALL_CONFIRM, INSTALLED, RENAME,
               ARCHIVE_SOURCE, ARCHIVE_DETAILS, ARCHIVED, ARCHIVES, RESTORE_DEST, RESTORE_CONFIRM, RESTORED } screen_t;
typedef struct { int port, unit; vmu_dir_t entry; char filename[13]; } save_t;
static save_t saves[MAX_FILES];
/* Only cache the visible page: 14 KiB, independent of card capacity. */
typedef struct { int key, valid; uint16_t pixels[32*32]; } save_icon_t;
static save_icon_t icons[7], remote_icons[7];
static int icon_cache_ready;
static unsigned little16(const unsigned char *p) {return p[0]|((unsigned)p[1]<<8);}
static void decode_icon(save_icon_t *icon,const unsigned char *h,size_t size) {
    icon->valid=0;
    if(size<640 || little16(h+64)<1 || little16(h+64)>3)return;
    for(int pixel=0;pixel<1024;++pixel) {
        unsigned pair=h[128+pixel/2];
        unsigned color=little16(h+96+2*((pixel&1)?pair&15:pair>>4));
        unsigned a=color>>12,r=(color>>8)&15,g=(color>>4)&15,b=color&15;
        unsigned red=(r*31*a/15+((PAPER>>11)&31)*(15-a))/15;
        unsigned green=(g*63*a/15+((PAPER>>5)&63)*(15-a))/15;
        unsigned blue=(b*31*a/15+(PAPER&31)*(15-a))/15;
        icon->pixels[pixel]=(red<<11)|(green<<5)|blue;
    }
    icon->valid=1;
}
static void draw_icon(const save_icon_t *icon,int x,int y) {
    for(int py=0;py<32;++py)for(int px=0;px<32;++px)
        vram_s[(y+py)*640+x+px]=icon->valid?icon->pixels[py*32+px]:
            (px>=3&&px<=28&&py>=3&&py<=28&&(px==3||px==28||py==3||py==28)?BLUE:PAPER);
}
static void draw_save_icon(int index,int x,int y) {
    int slot=index%7;
    if(!icon_cache_ready){for(int j=0;j<7;++j)icons[j].key=-1;icon_cache_ready=1;}
    if(icons[slot].key!=index) {
        save_t *s=&saves[index];void *raw=NULL;int size=0;
        maple_device_t *dev=maple_enum_dev(s->port,s->unit);
        icons[slot].key=index;icons[slot].valid=0;
        if(dev && (dev->info.functions&MAPLE_FUNC_MEMCARD) && vmufs_read(dev,s->filename,&raw,&size)==0) {
            size_t offset=(size_t)s->entry.hdroff*512;
            if(size>0 && offset+128+512<=(size_t)size && !auth_is_save(s->filename,raw,size)) {
                const unsigned char *h=(unsigned char *)raw+offset;
                unsigned frames=little16(h+64);
                if(frames>=1 && frames<=3 && offset+128+512*frames<=(size_t)size)
                    decode_icon(&icons[slot],h,size-offset);
            }
            memset(raw,0,size);free(raw);
        }
    }
    draw_icon(&icons[slot],x,y);
}

static int save_count, selected, focus, private_save, revision;
static int remembered;
static int card_ids[8], card_count, card_filter = -1;
static screen_t screen = STARTUP;
static remote_save_t remote_saves[7];
static int remote_count,remote_more,remote_page,remote_public,remote_selected;
static int remote_matching, replacement_id;
static char cloud_title[65];
static char browse_user[25], browse_game[81];
static int target_index,target_id,target_exists,target_old_size;
static vmu_root_t target_root;
static void *target_old;
/* Whole-card archives share one 128 KiB image buffer for capture and restore. */
static remote_archive_t archives[7];
static int archive_count,archive_more,archive_page,archive_selected;
static unsigned char *image;
static char archive_name[65];
static int archive_files,archive_card,restore_id,restore_files;
/* Per-card counts captured by destination_cards(). Redraws must never touch
   vmufs: block transfers hold its mutex while reporting progress. */
static int card_files[8],card_free[8];


static char username[25], password[129], token[96];
static char save_name[65], game[81], notes[501], status_text[160];
static void *save_data;
static int save_size;
static int edit_field = -1, osk_x, osk_y, dirty = 1, canceled;
static char edit_backup[501];
static uint32_t previous_buttons;
static const char *keys[] = {
    "abcdefghijklm", "nopqrstuvwxyz", "ABCDEFGHIJKLM", "NOPQRSTUVWXYZ",
    "0123456789_-.", "@!#$%&*+=?:/ "
};
static void draw(void);
void client_status(const char *message) {
    size_t i;
    snprintf(status_text, sizeof(status_text), "%s", message);
    for(i=0; status_text[i]; ++i) if((unsigned char)status_text[i]<32) status_text[i]=' ';
    dirty = 1;
}
static void text(int x, int y, uint16_t color, const char *value) {
    char clipped[50];
    snprintf(clipped, sizeof(clipped), "%.49s", value);
    bfont_draw_str_ex(vram_s + y * 640 + x, 640, color, PAPER, 16, false, clipped);
}
static void row(int index, const char *label, const char *value, int masked) {
    char line[100], hidden[42];
    size_t len = strlen(value);
    if(masked) { memset(hidden, '*', len>40?40:len); hidden[len>40?40:len]=0; value=hidden; }
    snprintf(line, sizeof(line), "%c %s: %s", focus==index?'>':' ', label, value);
    text(24, 116+index*36, focus==index?BLUE:INK, line);
}
static void action_row(int index, const char *label, const char *hint) {
    char line[100];
    snprintf(line, sizeof(line), "%c %s%s%s", focus==index?'>':' ', label,
             hint[0]?": ":"!", hint);
    text(24, 116+index*36, focus==index?BLUE:INK, line);
}
static char *field_value(size_t *capacity) {
    if(screen==LOGIN) {
        if(edit_field==0) { *capacity=sizeof(username); return username; }
        *capacity=sizeof(password); return password;
    }
    if(screen==PUBLIC_SEARCH) {
        if(edit_field==0){*capacity=sizeof(browse_user);return browse_user;}
        *capacity=sizeof(browse_game);return browse_game;
    }
    if(screen==RENAME){*capacity=sizeof(cloud_title);return cloud_title;}
    if(screen==ARCHIVE_DETAILS){*capacity=sizeof(archive_name);return archive_name;}
    if(screen==DETAILS && edit_field==0){*capacity=sizeof(save_name);return save_name;}
    *capacity=sizeof(notes); return notes;
}
static void stamp(long created,char *out,size_t capacity) {
    time_t when=(time_t)created;struct tm parts;
    if(!gmtime_r(&when,&parts) || !strftime(out,capacity,"%Y-%m-%d %H:%M",&parts))snprintf(out,capacity,"unknown date");
}
static void card_rows(int y) {
    char line[100];
    for(int i=0;i<card_count;++i) {
        if(card_files[i]<0)snprintf(line,sizeof(line),"%c VMU %c%d: unreadable",i==target_index?'>':' ','A'+card_ids[i]/6,card_ids[i]%6);
        else snprintf(line,sizeof(line),"%c VMU %c%d: %d files, %d free blocks",i==target_index?'>':' ','A'+card_ids[i]/6,card_ids[i]%6,card_files[i],card_free[i]);
        text(24,y+i*28,i==target_index?BLUE:INK,line);
    }
    if(!card_count)text(24,y+14,INK,"No VMUs attached. Insert one and rescan.");
}
static void draw(void) {
    int i;
    vid_waitvbl();
    for(i=0;i<640*480;++i) vram_s[i]=PAPER;
    text(24, 18, ORANGE, "DCVMU.com / Cloud Saves for Dreamcast");
    if(edit_field>=0) {
        size_t capacity; char *value=field_value(&capacity), display[501];
        (void)capacity;
        text(24,62,INK,"Edit field - keyboard or controller");
        if(screen==LOGIN && edit_field==1) {
            size_t n=strlen(value); memset(display,'*',n); display[n]=0;
        } else snprintf(display,sizeof(display),"%s",value);
        text(24,102,BLUE,strlen(display)>46?display+strlen(display)-46:display);
        for(i=0;i<6;++i) {
            int j;
            for(j=0;j<(int)strlen(keys[i]);++j) {
                char letter[4]={keys[i][j],0};
                if(keys[i][j]==' ') strcpy(letter,"_");
                text(36+j*42,148+i*32,(i==osk_y&&j==osk_x)?ORANGE:INK,letter);
            }
        }
        text(24,356,INK,"A add   X erase   Y done   B cancel");
        text(24,386,INK,"Enter done / Esc cancel / Tab done");
    } else if(screen==STARTUP) {
        text(24,66,INK,"Starting DCVMU...");
        text(24,150,INK,"Preparing network and checking saved login.");
        text(24,310,INK,"B / Esc cancels after the current dial step");
    } else if(screen==AUTO_LOGIN) {
        text(24,66,BLUE,"Attempting auto login...");
        text(24,150,INK,username);
        text(24,198,INK,"Checking your saved VMU session.");
        text(24,310,INK,"B / Esc cancel");
    } else if(screen==LOGIN) {
        text(24,66,INK,"Log in - register first at dcvmu.com");
        row(0,"Username",username,0); row(1,"Password",password,1);
        action_row(2,"Log in","");
        text(24,274,INK,"D-pad choose / A edit or continue");
        text(24,310,INK,"Keyboard: arrows, Enter, Tab");
        text(24,346,INK,"Login token saved to VMU; password in RAM.");
    } else if(screen==HOME) {
        text(24,66,INK,"What would you like to do?");
        action_row(0,"Upload a save","VMU to account");
        action_row(1,"Download a save","Account to VMU");
        action_row(2,"Archive a VMU","whole card to account");
        action_row(3,"Restore a VMU","account to whole card");
        action_row(4,"Browse Public Saves","");
        action_row(5,"Sign out","");
        text(24,346,INK,"A / Enter select   Start exits");
    } else if(screen==ARCHIVE_SOURCE) {
        text(24,66,INK,"Choose the VMU to archive");
        text(24,100,BLUE,"Every file on the card is saved as one snapshot.");
        card_rows(136);
        text(24,354,INK,"Your DCVMU login save is never archived.");
        text(24,382,INK,"A archive   Y/R rescan   B/Esc menu");
    } else if(screen==ARCHIVE_DETAILS) {
        char line[100];
        text(24,66,INK,"Archive details");
        row(0,"Archive name",archive_name[0]?archive_name:"(optional)",0);
        snprintf(line,sizeof(line),"VMU %c%d, %d files",'A'+archive_card/6,archive_card%6,archive_files);
        row(1,"Source (fixed)",line,0);
        action_row(2,"Archive VMU","");
        text(24,250,INK,"The date is added automatically.");
        text(24,282,INK,"Give it a friendly name to find it later.");
        text(24,328,INK,"A edit / archive   B back");
    } else if(screen==ARCHIVED) {
        text(24,80,BLUE,"Whole VMU archived successfully.");
        text(24,132,INK,"Restore it any time from Restore a VMU,");
        text(24,160,INK,"or manage it at dcvmu.com/archives.");
        text(24,212,INK,"A / Enter: archive another VMU");
        text(24,252,INK,"B / Esc: main menu   Start: exit");
    } else if(screen==ARCHIVES) {
        char line[100],date[24];
        snprintf(line,sizeof(line),"VMU archives - page %d",archive_page+1);
        text(24,66,INK,line);
        if(!archive_count)text(24,116,INK,"No archives on this page.");
        for(i=0;i<archive_count;++i) {
            stamp(archives[i].created,date,sizeof(date));
            snprintf(line,sizeof(line),"%c %-26.26s %s",i==archive_selected?'>':' ',archives[i].name,date);
            text(24,108+i*34,i==archive_selected?BLUE:INK,line);
        }
        if(archive_count) {
            snprintf(line,sizeof(line),"%s%s%d files in this archive",archives[archive_selected].source,archives[archive_selected].source[0]?": ":"",archives[archive_selected].files);
            text(24,326,INK,line);
        }
        snprintf(line,sizeof(line),"%s%sY/R refresh",archive_page?"Left: previous   ":"",archive_more?"Right: next   ":"");
        text(24,354,INK,line);
        text(24,382,INK,"A restore   B/Esc menu");
    } else if(screen==RESTORE_DEST) {
        char line[100];text(24,66,INK,"Choose the VMU to restore onto");
        snprintf(line,sizeof(line),"%.36s (%d files)",archives[archive_selected].name,archives[archive_selected].files);
        text(24,100,BLUE,line);
        card_rows(136);
        text(24,354,INK,"Nothing is written until you confirm.");
        text(24,382,INK,"A select   Y/R rescan   B/Esc back");
    } else if(screen==RESTORE_CONFIRM) {
        char line[100];text(24,66,ORANGE,"Erase this VMU and restore the archive?");
        snprintf(line,sizeof(line),"%.28s -> VMU %c%d",archives[archive_selected].name,'A'+restore_id/6,restore_id%6);
        text(24,112,INK,line);
        snprintf(line,sizeof(line),"All %d files on VMU %c%d will be replaced.",restore_files,'A'+restore_id/6,restore_id%6);
        text(24,150,INK,line);
        text(24,180,INK,"Archive this VMU first if you may want it back.");
        text(24,222,focus==0?BLUE:INK,focus==0?"> Cancel":"  Cancel");
        text(24,258,focus==1?BLUE:INK,focus==1?"> Restore whole VMU":"  Restore whole VMU");
        text(24,330,INK,"Keep the VMU inserted while writing.");
        text(24,382,INK,"Up/Down choose   A confirm   B/Esc back");
    } else if(screen==RESTORED) {
        text(24,80,BLUE,"Whole VMU restored and verified.");
        text(24,150,INK,archives[archive_selected].name);
        text(24,190,INK,"Reinsert the VMU to refresh its menu.");
        text(24,240,INK,"A / Enter main menu   B / Esc archives");
    } else if(screen==PUBLIC_SEARCH) {
        text(24,66,INK,"Browse Public Saves");
        row(0,"Username",browse_user,0);
        row(1,"Game (optional)",browse_game,0);
        action_row(2,"Find saves","");
        text(24,250,INK,"Discover saves at dcvmu.com first.");
        text(24,282,INK,"Enter the owner's exact username here.");
        text(24,314,INK,"Game matches part of the game title.");
        text(24,382,INK,"A edit / search   B/Esc menu");
    } else if(screen==DOWNLOADS) {
        char line[100];
        if(remote_matching)snprintf(line,sizeof(line),"Choose backup to replace - page %d",remote_page+1);
        else if(remote_public)snprintf(line,sizeof(line),"Public: %s - page %d",browse_user,remote_page+1);
        else snprintf(line,sizeof(line),"My saves - page %d",remote_page+1);
        text(24,66,INK,line);
        if(!remote_count) {
            text(24,116,INK,"No saves on this page.");
            if(remote_public)text(24,152,INK,"B: check username or change game filter.");
        }
        for(i=0;i<remote_count;++i) {
            draw_icon(&remote_icons[i],24,106+i*34);
            snprintf(line,sizeof(line),"%c %.41s",i==remote_selected?'>':' ',remote_saves[i].name);
            text(64,108+i*34,i==remote_selected?BLUE:INK,line);
        }
        if(remote_count)text(24,326,INK,remote_saves[remote_selected].game);
        snprintf(line,sizeof(line),"%s%sY/R refresh",remote_page?"Left: previous   ":"",remote_more?"Right: next   ":"");
        text(24,354,INK,line);
        text(24,382,INK,remote_matching?"A choose   X/N keep new   B back":remote_public?"A select   B/Esc change search":"A download   X/N rename   B/Esc menu");
    } else if(screen==RENAME) {
        text(24,66,INK,"Rename cloud save");
        row(0,"Save title",cloud_title,0);
        row(1,"Game (fixed)",remote_saves[remote_selected].game,0);
        row(2,"VMU filename",remote_saves[remote_selected].filename,0);
        action_row(3,"Save title","");
        text(24,328,INK,"A edit / save   B cancel");
        text(24,366,INK,"Only the archive title changes.");
    } else if(screen==DESTINATION) {
        char line[100];text(24,66,INK,"Choose destination VMU");
        snprintf(line,sizeof(line),"%.32s (%d blocks)",remote_saves[remote_selected].filename,remote_saves[remote_selected].size/512);
        text(24,100,BLUE,line);
        for(i=0;i<card_count;++i) {
            maple_device_t *dev=maple_enum_dev(card_ids[i]/6,card_ids[i]%6);
            int blocks=dev?vmufs_free_blocks(dev):-1;
            snprintf(line,sizeof(line),"%c VMU %c%d: %d free blocks",i==target_index?'>':' ','A'+card_ids[i]/6,card_ids[i]%6,blocks);
            text(24,136+i*28,i==target_index?BLUE:INK,line);
        }
        if(!card_count)text(24,150,INK,"No VMUs attached. Insert one and rescan.");
        text(24,382,INK,"A select   Y/R rescan   B/Esc back");
    } else if(screen==INSTALL_CONFIRM) {
        int custom=!strcmp(remote_saves[remote_selected].filename,DCVMU_ICON_FILE);
        char line[100];text(24,66,ORANGE,custom?(target_exists?"Replace this VMU's custom icons?":"Apply custom icons to this VMU?"):(target_exists?"Replace the existing VMU save?":"Install this save on the VMU?"));
        snprintf(line,sizeof(line),"%.12s -> VMU %c%d",remote_saves[remote_selected].filename,'A'+target_id/6,target_id%6);
        text(24,112,INK,line);
        text(24,150,INK,custom?"Changes the VMU and Dreamcast menu icons.":target_exists?"The existing save will be overwritten.":"Other saves will not be changed.");
        if(custom)text(24,180,INK,"Game saves are kept. Restart to see icons.");
        text(24,222,focus==0?BLUE:INK,focus==0?"> Cancel":"  Cancel");
        text(24,258,focus==1?BLUE:INK,focus==1?(target_exists?"> Replace save":"> Install save"):(target_exists?"  Replace save":"  Install save"));
        text(24,330,INK,"Keep the VMU inserted while writing.");
        text(24,382,INK,"Up/Down choose   A confirm   B/Esc back");
    } else if(screen==INSTALLED) {
        text(24,80,BLUE,!strcmp(remote_saves[remote_selected].filename,DCVMU_ICON_FILE)?"Custom icons installed and verified.":"Save installed and verified.");
        text(24,150,INK,remote_saves[remote_selected].filename);
        text(24,230,INK,"A / Enter main menu   B / Esc downloads");
    } else if(screen==FILES) {
        char line[100];
        if(card_filter<0) snprintf(line,sizeof(line),"All VMUs: %d cards, %d saves",card_count,save_count);
        else snprintf(line,sizeof(line),"VMU %c%d: %d saves",'A'+card_filter/6,card_filter%6,save_count);
        text(24,66,INK,line);
        if(!save_count) text(24,116,INK,card_count?"No saves here. Select another VMU.":"No VMUs attached. Insert a VMU.");
        for(i=selected/7*7;i<save_count && i<selected/7*7+7;++i) {
            snprintf(line,sizeof(line),"%c %c%d  %-12s  %u blocks",i==selected?'>':' ',
                     'A'+saves[i].port,saves[i].unit,saves[i].filename,saves[i].entry.filesize);
            draw_save_icon(i,24,110+(i%7)*34);
            text(64,112+(i%7)*34,i==selected?BLUE:INK,line);
        }
        text(24,354,INK,"Left / Right: VMU   A / Enter: select");
        text(24,382,INK,"Y/R rescan   B/Esc menu   Start exits");
    } else if(screen==DETAILS) {
        text(24,66,INK,"Upload details");
        text(24,88,BLUE,saves[selected].filename);
        draw_save_icon(selected,568,62);
        row(0,"Save title",save_name,0); row(1,"Game (fixed)",game,0); row(2,"Notes",notes,0);
        row(3,"Visibility",private_save?"PRIVATE - only you":"PUBLIC - everyone",0);
        action_row(4,"Upload","");
        text(24,328,INK,"A edit / toggle / upload   B back");
        text(24,366,INK,"Duplicates always ask before replacing.");
    } else if(screen==CONFLICT) {
        text(24,66,ORANGE,"Replace this cloud backup?");
        text(24,88,BLUE,remote_saves[remote_selected].name);
        text(24,112,INK,"Choose what to do with your upload:");
        action_row(1,"Replace existing", "A button / Enter");
        action_row(2,"Keep both", "Y button / K");
        text(24,268,INK,"B / Backspace: choose another backup");
        text(24,308,INK,"Keeps its title; uses new notes/visibility.");
    } else {
        text(24,80,BLUE,"Save uploaded successfully.");
        text(24,132,INK,"Visit dcvmu.com to view your archive.");
        text(24,192,INK,"A / Enter: upload another save");
        text(24,232,INK,"B / Esc: main menu   Start: exit");
    }
    char first[50];
    size_t split=strlen(status_text);
    if(split>48) { split=48; while(split>20&&status_text[split]!=' ')split--; }
    memcpy(first,status_text,split); first[split]=0;
    text(24,412,BLUE,first);
    if(strlen(status_text)>split)text(24,438,BLUE,status_text+split+(status_text[split]==' '));
    dirty=0;
}
static int poll_cancel(void) {
    maple_device_t *dev=maple_enum_type(0,MAPLE_FUNC_CONTROLLER);
    cont_state_t *state=dev?maple_dev_status(dev):NULL;
    if(state) {
        uint32_t pressed=state->buttons & ~previous_buttons;
        previous_buttons=state->buttons;
        if(pressed & (CONT_B|CONT_START)) canceled=1;
    }
    dev=maple_enum_type(0,MAPLE_FUNC_KEYBOARD);
    if(dev) { int raw; while((raw=kbd_queue_pop(dev,0))!=KBD_QUEUE_END)
        if((raw&255)==KBD_KEY_ESCAPE) canceled=1; }
    return canceled;
}
int client_connect_update(const char *message) {
    poll_cancel();
    client_status(canceled?"Cancel requested. Waiting for modem step to finish...":message);
    draw();
    return canceled;
}
void archive_progress(const char *stage,int done,int total) {
    snprintf(status_text,sizeof(status_text),"%s: %d/%d blocks - keep the card inserted",stage,done,total);
    draw();
}
int transfer_update(uint64_t done, uint64_t total) {
    static uint64_t last;
    uint64_t now=timer_ms_gettime64();
    poll_cancel();
    if(now-last>200) {
        snprintf(status_text,sizeof(status_text),"HTTPS %lu/%lu KB - B/Esc cancel",(unsigned long)(done/1024),(unsigned long)((total+1023)/1024));
        draw(); last=now;
    }
    return canceled;
}
static void scan(void) {
    int n;
    save_count=0; selected=0; card_count=0;icon_cache_ready=0;
    for(n=0;;++n) {
        maple_device_t *dev=maple_enum_type(n,MAPLE_FUNC_MEMCARD);
        vmu_dir_t *entries=NULL; int count=0,i;
        if(!dev) break;
        int card_id=dev->port*6+dev->unit;
        if(card_count<8) card_ids[card_count++]=card_id;
        if(card_filter>=0 && card_filter!=card_id) continue;
        if(vmufs_readdir(dev,&entries,&count)<0) { printf("dcvmu: cannot read VMU %c%d\n",'A'+dev->port,dev->unit); continue; }
        for(i=0;i<count && save_count<MAX_FILES;++i) {
            save_t *out;
            if(entries[i].filetype!=0x33 || !entries[i].filesize) continue;
            char filename[13];memcpy(filename,entries[i].filename,12);filename[12]=0;
            for(int j=11;j>=0 && filename[j]==' ';--j)filename[j]=0;
            if(auth_is_save(filename,NULL,0) || !strcmp(filename,DCVMU_ICON_FILE))continue;
            out=&saves[save_count++]; out->port=dev->port; out->unit=dev->unit; out->entry=entries[i];
            memcpy(out->filename,entries[i].filename,12); out->filename[12]=0;
            for(int j=11;j>=0 && out->filename[j]==' ';--j) out->filename[j]=0;
        }
        free(entries);
    }
    printf("dcvmu: scanned %d data saves\n",save_count);
    client_status(save_count?"Select a save to upload.":"No saves. Y/R rescans inserted VMUs.");
}
static int load_downloads(void);
static int load_archives(void);
static void switch_card(int delta) {
    if(screen==DOWNLOADS) {
        int previous_page=remote_page;
        if(delta<0 && remote_page>0)--remote_page;
        else if(delta>0 && remote_more)++remote_page;
        else return;
        if(load_downloads()<0)remote_page=previous_page;
        return;
    }
    if(screen==ARCHIVES) {
        int previous_page=archive_page;
        if(delta<0 && archive_page>0)--archive_page;
        else if(delta>0 && archive_more)++archive_page;
        else return;
        if(load_archives()<0)archive_page=previous_page;
        return;
    }
    int index=0;
    if(screen!=FILES)return;
    for(int i=0;i<card_count;++i)if(card_ids[i]==card_filter)index=i+1;
    index=(index+delta+card_count+1)%(card_count+1);
    card_filter=index?card_ids[index-1]:-1;
    scan(); dirty=1;
}
static void choose_save(void) {
    save_t *s; maple_device_t *dev; unsigned char *bytes;
    if(!save_count) return;
    s=&saves[selected]; dev=maple_enum_dev(s->port,s->unit);
    free(save_data); save_data=NULL;
    if(!dev || !(dev->info.functions & MAPLE_FUNC_MEMCARD) ||
       vmufs_read(dev,s->filename,&save_data,&save_size)<0) { client_status("VMU changed or read failed. Rescan."); return; }
    if(save_size<=0 || save_size>131072 || save_size%512) { free(save_data);save_data=NULL;client_status("Invalid VMU save size.");return; }
    if(auth_is_save(s->filename,save_data,save_size)) {
        memset(save_data,0,save_size);free(save_data);save_data=NULL;
        client_status("Private login saves cannot be uploaded.");return;
    }
    snprintf(save_name,sizeof(save_name),"%s",s->filename);
    snprintf(game,sizeof(game),"%s",s->filename);
    bytes=save_data;
    if((unsigned)s->entry.hdroff*512+48<=(unsigned)save_size) {
        size_t offset=(unsigned)s->entry.hdroff*512+16;
        int j; for(j=0;j<32;++j) game[j]=bytes[offset+j]>=32&&bytes[offset+j]<127?bytes[offset+j]:' ';
        game[32]=0; for(j=31;j>=0&&game[j]==' ';--j)game[j]=0;
        if(!game[0])snprintf(game,sizeof(game),"%s",s->filename);
    }
    notes[0]=0;private_save=0;revision=0;replacement_id=remote_matching=0;focus=0;screen=DETAILS;
    printf("dcvmu: read %s (%d bytes), VMU unchanged\n",s->filename,save_size);
    client_status("Set a title, notes and visibility before upload.");
}
static void upload(const char *mode) {
    int result;
    canceled=0;client_status("Connecting securely...");draw();
    result=service_upload(token,save_name,saves[selected].filename,game,notes,private_save,
                          save_data,(size_t)save_size,saves[selected].entry.hdroff,mode,replacement_id,&revision);
    if(result==1 || result==2) {
        remote_matching=1;remote_public=0;remote_page=0;replacement_id=0;
        if(load_downloads()<0){screen=DETAILS;remote_matching=0;}
    }
    else if(result==0) { screen=SUCCESS;free(save_data);save_data=NULL; }
    dirty=1;
}
static void edit_begin(int field) {
    size_t capacity; char *value;
    edit_field=field;value=field_value(&capacity);
    snprintf(edit_backup,sizeof(edit_backup),"%s",value);osk_x=osk_y=0;dirty=1;
}
static void edit_end(int cancel) {
    size_t capacity;char *value=field_value(&capacity);
    if(cancel)snprintf(value,capacity,"%s",edit_backup);
    memset(edit_backup,0,sizeof(edit_backup));edit_field=-1;dirty=1;
}
static void edit_char(char c) {
    size_t capacity;char *value=field_value(&capacity);size_t n=strlen(value);
    if(c=='\b') { if(n)value[n-1]=0; }
    else if(c>=32&&c<=126&&n+1<capacity) {value[n]=c;value[n+1]=0;}
    dirty=1;
}
static void sign_out(void) {
    canceled=0;service_logout(token);
    int result=auth_forget();
    remembered=0;memset(token,0,sizeof(token));screen=LOGIN;focus=0;dirty=1;
    client_status(result==0?"Signed out; saved login removed.":"Signed out. Reinsert VMU to remove login save.");
}
static int load_downloads(void) {
    remote_save_t items[7]={0};int count=0,more=0;
    canceled=0;client_status("Loading saves...");draw();
    int listed=remote_matching?service_matches(token,saves[selected].filename,remote_page,items,&count,&more):
        service_list(token,remote_public?browse_user:NULL,remote_public?browse_game:NULL,remote_page,items,&count,&more);
    if(listed==0 && remote_matching)for(int i=0;i<count;++i)if(strcasecmp(items[i].user,username)) {
        client_status("Unexpected backup owner. Refresh and retry.");return -1;
    }
    if(listed==0) {
        /* Commit only a complete list. Failed/canceled page turns keep the
           previous list, selection and icons available for a retry. */
        memcpy(remote_saves,items,sizeof(items));remote_count=count;remote_more=more;remote_selected=0;
        memset(remote_icons,0,sizeof(remote_icons));screen=DOWNLOADS;
        client_status("Loading save icons...");draw();
        for(int i=0;i<remote_count;++i) {
            unsigned char header[640];
            if(service_icon(token,&remote_saves[i],header)<0)break;
            decode_icon(&remote_icons[i],header,sizeof(header));
            dirty=1;draw();
        }
        client_status(!remote_count?(remote_public?"No public saves match. Check username and game.":"You have no saves on this page."):
                      remote_matching?"Choose a backup, or X/N to keep a new copy.":canceled?"Icon loading canceled. Select a save.":"Select a save to download.");
    }
    dirty=1;return listed;
}
static void refresh_downloads(void) {
    int previous_page=remote_page;remote_page=0;
    if(load_downloads()<0)remote_page=previous_page;
}
static void destination_cards(void) {
    card_count=0;target_index=0;
    for(int n=0;n<8;++n) {
        maple_device_t *dev=maple_enum_type(n,MAPLE_FUNC_MEMCARD);if(!dev)break;
        card_ids[card_count]=dev->port*6+dev->unit;
        card_files[card_count]=archive_card_files(dev);
        card_free[card_count]=vmufs_free_blocks(dev);
        ++card_count;
    }
    dirty=1;
}
static int read_root(maple_device_t *dev,vmu_root_t *root) {
    if(vmufs_mutex_lock()<0)return -1;
    int result=vmufs_root_read(dev,root);vmufs_mutex_unlock();return result;
}
static int find_target(maple_device_t *dev,vmu_dir_t *entry) {
    vmu_dir_t *entries=NULL;int count=0,found=0;
    if(vmufs_readdir(dev,&entries,&count)<0)return -1;
    for(int i=0;i<count;++i) {
        char name[13];memcpy(name,entries[i].filename,12);name[12]=0;
        for(int j=11;j>=0 && name[j]==' ';--j)name[j]=0;
        if(entries[i].filetype && !strcmp(name,remote_saves[remote_selected].filename)) {
            *entry=entries[i];found=1;break;
        }
    }
    free(entries);return found;
}
static void prepare_install(void) {
    if(!card_count)return;
    free(target_old);target_old=NULL;target_old_size=0;
    target_id=card_ids[target_index];
    maple_device_t *dev=maple_enum_dev(target_id/6,target_id%6);vmu_dir_t entry;
    if(!dev || !(dev->info.functions&MAPLE_FUNC_MEMCARD) || read_root(dev,&target_root)<0)goto failure;
    target_exists=find_target(dev,&entry);
    if(target_exists<0)goto failure;
    if(target_exists && (entry.filetype!=0x33 || vmufs_read_dirent(dev,&entry,&target_old,&target_old_size)<0))goto failure;
    int available=vmufs_free_blocks(dev);
    if(available<0)goto failure;
    if(available+(target_exists?entry.filesize:0)<remote_saves[remote_selected].size/512) {
        client_status("Not enough free blocks. Choose another VMU.");return;
    }
    focus=0;screen=INSTALL_CONFIRM;dirty=1;return;
failure:client_status("Could not read destination VMU. Rescan.");
}
static void install_download(void) {
    maple_device_t *dev=maple_enum_dev(target_id/6,target_id%6);vmu_root_t root;vmu_dir_t entry;
    void *current=NULL;int current_size=0;
    remote_save_t *item=&remote_saves[remote_selected];
    if(!dev || !(dev->info.functions&MAPLE_FUNC_MEMCARD) || read_root(dev,&root)<0 || memcmp(&root,&target_root,sizeof(root)))goto changed;
    int exists=find_target(dev,&entry);
    if(exists<0 || exists!=target_exists)goto changed;
    if(exists) {
        if(vmufs_read_dirent(dev,&entry,&current,&current_size)<0)goto changed;
        int same=current_size==target_old_size && !memcmp(current,target_old,current_size);
        free(current);current=NULL;if(!same)goto changed;
    }
    int available=vmufs_free_blocks(dev);
    if(available<0 || available+(exists?entry.filesize:0)<item->size/512)goto changed;
    client_status("Writing VMU. Do not remove the card.");draw();
    int result=vmufs_write(dev,item->filename,save_data,item->size,exists?VMUFS_OVERWRITE:0);
    if(result==0 && item->header_offset) {
        /* readdir compacts entries; raw directory I/O must use the complete table. */
        if(vmufs_mutex_lock()<0)result=-1;
        else {
            int bytes=vmufs_dir_blocks(&root);
            vmu_dir_t *entries=bytes>0 && bytes<=131072?malloc(bytes):NULL;
            int found=0;
            if(!entries || vmufs_dir_read(dev,&root,entries)<0)result=-1;
            else {
                for(int i=0;i<bytes/(int)sizeof(*entries);++i)if(entries[i].filetype && !strncmp(entries[i].filename,item->filename,12)) {
                    entries[i].hdroff=item->header_offset;entries[i].dirty=1;found=1;
                }
                result=found?vmufs_dir_write(dev,&root,entries):-1;
            }
            free(entries);vmufs_mutex_unlock();
        }
    }
    if(result==0 && vmufs_read(dev,item->filename,&current,&current_size)==0 &&
       current_size==item->size && !memcmp(current,save_data,current_size)) {
        screen=INSTALLED;
        client_status("Installed. VMU read-back matches download.");
    } else client_status("VMU write/verification failed. Keep card inserted.");
    free(current);free(target_old);target_old=NULL;dirty=1;return;
changed:
    free(current);screen=DESTINATION;destination_cards();client_status("VMU changed or is full. Select it again.");
}
static int load_archives(void) {
    remote_archive_t items[7]={0};int count=0,more=0;
    canceled=0;client_status("Loading VMU archives...");draw();
    int listed=service_archive_list(token,archive_page,items,&count,&more);
    if(listed==0) {
        memcpy(archives,items,sizeof(items));archive_count=count;archive_more=more;archive_selected=0;screen=ARCHIVES;
        client_status(count?"Select an archive to restore.":"No VMU archives yet. Archive a VMU first.");
    }
    dirty=1;return listed;
}
static void refresh_archives(void) {
    int previous_page=archive_page;archive_page=0;
    if(load_archives()<0)archive_page=previous_page;
}
static int image_buffer(void) {
    if(!image)image=malloc(ARCHIVE_IMAGE_SIZE);
    if(!image)client_status("Not enough memory for a whole-card image.");
    return image?0:-1;
}
static void archive_capture(void) {
    if(!card_count || image_buffer()<0)return;
    int id=card_ids[target_index];
    maple_device_t *dev=maple_enum_dev(id/6,id%6);
    if(!dev || !(dev->info.functions&MAPLE_FUNC_MEMCARD)){client_status("VMU changed. Rescan.");return;}
    client_status("Reading the whole VMU. Keep the card inserted.");draw();
    if(archive_read(dev,image)<0){client_status("Could not read every block. Rescan and retry.");return;}
    if(archive_scrub(image,&archive_files)<0){memset(image,0,ARCHIVE_IMAGE_SIZE);client_status("This VMU is not a standard formatted card.");return;}
    archive_card=id;archive_name[0]=0;focus=0;screen=ARCHIVE_DETAILS;dirty=1;
    printf("dcvmu: captured VMU %c%d (%d files), login save excluded\n",'A'+id/6,id%6,archive_files);
    client_status("Card copied. Name the archive, then upload it.");
}
static void archive_upload(void) {
    char source[25];
    snprintf(source,sizeof(source),"VMU %c%d",'A'+archive_card/6,archive_card%6);
    canceled=0;client_status("Uploading the whole VMU securely...");draw();
    if(service_archive_upload(token,archive_name,source,image,ARCHIVE_IMAGE_SIZE)==0)screen=ARCHIVED;
    dirty=1;
}
static void prepare_restore(void) {
    if(!card_count)return;
    restore_id=card_ids[target_index];
    maple_device_t *dev=maple_enum_dev(restore_id/6,restore_id%6);
    if(!dev || !(dev->info.functions&MAPLE_FUNC_MEMCARD) || (restore_files=archive_card_files(dev))<0) {
        client_status("Could not read destination VMU. Rescan.");return;
    }
    focus=0;screen=RESTORE_CONFIRM;dirty=1;
}
static void restore_image(void) {
    maple_device_t *dev=maple_enum_dev(restore_id/6,restore_id%6);
    if(!dev || !(dev->info.functions&MAPLE_FUNC_MEMCARD)){screen=RESTORE_DEST;destination_cards();client_status("VMU changed. Select it again.");return;}
    unsigned char *readback=malloc(ARCHIVE_IMAGE_SIZE);
    if(!readback){client_status("Not enough memory to verify the VMU.");return;}
    client_status("Writing the whole VMU. Do not remove the card.");draw();
    int result=archive_write(dev,image,readback);
    memset(readback,0,ARCHIVE_IMAGE_SIZE);free(readback);
    if(result==0) {
        /* The archive never contains a login save; put ours back on its card. */
        int login=remembered && auth_location()==restore_id;
        int rewritten=login && auth_save(username,token)==0;
        screen=RESTORED;
        client_status(!login?"Restored. VMU read-back matches the archive.":rewritten?"Restored and verified. Login save rewritten.":"Restored and verified. Log in again next time.");
        printf("dcvmu: restored archive %d onto VMU %c%d\n",archives[archive_selected].id,'A'+restore_id/6,restore_id%6);
    } else client_status(result==-2?"Read-back mismatch. Restore again before using this VMU.":"VMU write failed. Restore again before using this VMU.");
    dirty=1;
}
static void activate(void) {
    if(screen==LOGIN) {
        if(focus<2)edit_begin(focus);
        else if(username[0]&&password[0]) {
            canceled=0;client_status("Logging in securely...");draw();
            if(service_login(username,password,token,sizeof(token))==0) {remembered=auth_save(username,token)==0;screen=HOME;focus=0;
                client_status(remembered?"Login remembered on VMU.":"Login works, but VMU login save failed.");}
            memset(password,0,sizeof(password));dirty=1;
        } else client_status("Enter username and password.");
    } else if(screen==HOME) {
        if(focus==0){screen=FILES;scan();}
        else if(focus==1){remote_page=remote_public=remote_matching=0;load_downloads();}
        else if(focus==2){screen=ARCHIVE_SOURCE;destination_cards();client_status("Choose the VMU to archive.");}
        else if(focus==3){archive_page=0;load_archives();}
        else if(focus==4){screen=PUBLIC_SEARCH;focus=0;client_status("Enter the save owner's username.");}
        else sign_out();
    } else if(screen==ARCHIVE_SOURCE)archive_capture();
    else if(screen==ARCHIVE_DETAILS) {
        if(focus==0)edit_begin(0);else archive_upload();
    } else if(screen==ARCHIVED){screen=ARCHIVE_SOURCE;destination_cards();client_status("Choose the VMU to archive.");}
    else if(screen==ARCHIVES) {
        if(archive_count && image_buffer()==0) {
            canceled=0;client_status("Downloading the archive...");draw();
            if(service_archive_download(token,&archives[archive_selected],image)==0) {
                screen=RESTORE_DEST;destination_cards();client_status("Choose a VMU to overwrite. Nothing written yet.");
            }
        }
    } else if(screen==RESTORE_DEST)prepare_restore();
    else if(screen==RESTORE_CONFIRM) {
        if(focus==0)screen=RESTORE_DEST;else restore_image();
    } else if(screen==RESTORED){screen=HOME;focus=0;}
    else if(screen==PUBLIC_SEARCH) {
        if(focus<2)edit_begin(focus);
        else {remote_page=0;remote_public=1;remote_matching=0;load_downloads();}
    } else if(screen==DOWNLOADS) {
        if(remote_matching) {
            if(remote_count){replacement_id=remote_saves[remote_selected].id;revision=remote_saves[remote_selected].revision;screen=CONFLICT;focus=1;}
        } else if(remote_count) {
            canceled=0;free(save_data);save_data=NULL;
            if(service_download(token,&remote_saves[remote_selected],&save_data)==0) {
                screen=DESTINATION;destination_cards();client_status("Select a destination. Nothing written yet.");
            }
        }
    } else if(screen==RENAME) {
        if(focus==0)edit_begin(0);
        else if(cloud_title[0]) {
            canceled=0;
            if(service_rename(token,&remote_saves[remote_selected],cloud_title)==0) {
                screen=DOWNLOADS;load_downloads();client_status("Save title updated. VMU unchanged.");
            }
        } else client_status("Enter a save title.");
    } else if(screen==DESTINATION)prepare_install();
    else if(screen==INSTALL_CONFIRM) {
        if(focus==0)screen=DESTINATION;else install_download();
    } else if(screen==INSTALLED) {free(save_data);save_data=NULL;screen=HOME;focus=0;}
    else if(screen==FILES)choose_save();
    else if(screen==DETAILS) {
        if(focus==0 || focus==2)edit_begin(focus);
        else if(focus==3)private_save=!private_save;
        else if(save_name[0]&&game[0])upload("ask");
        else client_status("A save title is required.");
    } else if(screen==CONFLICT)upload("replace");
    else {screen=FILES;scan();}
    dirty=1;
}
static void cloud_action(void) {
    if(screen!=DOWNLOADS)return;
    if(remote_matching){replacement_id=0;upload("keep");return;}
    if(remote_public || !remote_count)return;
    snprintf(cloud_title,sizeof(cloud_title),"%s",remote_saves[remote_selected].name);
    screen=RENAME;focus=0;client_status("Edit the title, then choose Save title.");
}
static void move(int delta) {
    if(screen==FILES && save_count)selected=(selected+delta+save_count)%save_count;
    else if(screen==DOWNLOADS && remote_count)remote_selected=(remote_selected+delta+remote_count)%remote_count;
    else if((screen==DESTINATION || screen==ARCHIVE_SOURCE || screen==RESTORE_DEST) && card_count)target_index=(target_index+delta+card_count)%card_count;
    else if(screen==ARCHIVES && archive_count)archive_selected=(archive_selected+delta+archive_count)%archive_count;
    else if(screen==INSTALL_CONFIRM || screen==RESTORE_CONFIRM)focus=(focus+delta+2)%2;
    else if(screen==ARCHIVE_DETAILS)focus=focus==0?2:0;
    else if(screen==HOME)focus=(focus+delta+6)%6;
    else if(screen==PUBLIC_SEARCH)focus=(focus+delta+3)%3;
    else if(screen==LOGIN)focus=(focus+delta+3)%3;
    else if(screen==RENAME)focus=focus==0?3:0;
    else if(screen==DETAILS) {
        static const int fields[]={0,2,3,4};int index=0;
        while(index<3 && fields[index]!=focus)++index;
        focus=fields[(index+delta+4)%4];
    }
    dirty=1;
}
static void back(void) {
    if(edit_field>=0)edit_end(1);
    else if(screen==INSTALL_CONFIRM)screen=DESTINATION;
    else if(screen==DESTINATION || screen==INSTALLED){screen=DOWNLOADS;free(save_data);save_data=NULL;free(target_old);target_old=NULL;}
    else if(screen==RENAME){screen=DOWNLOADS;client_status("Rename canceled.");}
    else if(screen==ARCHIVE_SOURCE){screen=HOME;focus=2;client_status("");}
    else if(screen==ARCHIVE_DETAILS){screen=ARCHIVE_SOURCE;destination_cards();client_status("Archive canceled. Nothing uploaded.");}
    else if(screen==ARCHIVED){screen=HOME;focus=2;client_status("");}
    else if(screen==ARCHIVES){screen=HOME;focus=3;client_status("");}
    else if(screen==RESTORE_DEST || screen==RESTORED){screen=ARCHIVES;client_status(screen==ARCHIVES?"Select an archive to restore.":"");}
    else if(screen==RESTORE_CONFIRM){screen=RESTORE_DEST;client_status("Nothing written.");}
    else if(screen==DOWNLOADS && remote_matching){screen=DETAILS;remote_matching=0;replacement_id=0;focus=0;}
    else if(screen==DOWNLOADS && remote_public){screen=PUBLIC_SEARCH;focus=2;client_status("Edit the search or find saves again.");}
    else if(screen==PUBLIC_SEARCH){screen=HOME;focus=4;client_status("");}
    else if(screen==FILES || screen==DOWNLOADS || screen==SUCCESS){screen=HOME;focus=0;client_status("");}
    else if(screen==CONFLICT){screen=DOWNLOADS;replacement_id=0;}
    else if(screen==DETAILS) {screen=FILES;free(save_data);save_data=NULL;}
    dirty=1;
}
#ifdef DCVMU_SELF_TEST
static void run_self_test(void) {
    FILE *credentials=fopen("/rd/test-credentials.txt","r");
    if(!credentials||!fgets(username,sizeof(username),credentials)||!fgets(password,sizeof(password),credentials)) {
        printf("dcvmu: SELF-TEST FAILED credentials missing\n");if(credentials)fclose(credentials);return;
    }
    fclose(credentials);username[strcspn(username,"\r\n")]=0;password[strcspn(password,"\r\n")]=0;
    screen=LOGIN;focus=2;activate();
    if(screen!=HOME)goto fail;
    focus=0;activate();
    for(selected=0;selected<save_count;++selected)if(!strcmp(saves[selected].filename,"DCVMU_TEST"))break;
    if(selected>=save_count)goto fail;
    choose_save();if(screen!=DETAILS)goto fail;
    snprintf(notes,sizeof(notes),"Synthetic Flycast end-to-end verification");
    printf("dcvmu: VMU READ SELF-TEST PASS (%d bytes)\n",save_size);
    upload("ask");
    if(screen==SUCCESS) {
        printf("dcvmu: PUBLIC UPLOAD SELF-TEST PASS\n");
        choose_save();upload("ask");
    }
    if(screen==DOWNLOADS && remote_matching)activate();
    if(screen!=CONFLICT||revision<1)goto fail;
    int previous_revision=revision;
    printf("dcvmu: DUPLICATE CONFLICT SELF-TEST PASS\n");
    private_save=1;upload("replace");if(screen!=SUCCESS)goto fail;
    printf("dcvmu: PRIVATE REPLACE SELF-TEST PASS\n");
    choose_save();upload("ask");if(screen==DOWNLOADS && remote_matching)activate();if(screen!=CONFLICT||revision!=previous_revision+1)goto fail;
    upload("keep");if(screen!=SUCCESS)goto fail;
    printf("dcvmu: KEEP BOTH SELF-TEST PASS\n");
    printf("dcvmu: SELF-TEST PASSED\n");return;
fail:printf("dcvmu: SELF-TEST FAILED screen=%d status=%s\n",screen,status_text);
}
#endif

#if defined(DCVMU_DOWNLOAD_TEST) && !defined(DCVMU_PUBLIC_TEST) && !defined(DCVMU_TOOLS_TEST)
static void run_download_test(void) {
    FILE *credentials=fopen("/rd/test-credentials.txt","r");
    if(!credentials)return;
    if(!fgets(username,sizeof(username),credentials)||!fgets(password,sizeof(password),credentials)){fclose(credentials);return;}
    fclose(credentials);username[strcspn(username,"\r\n")]=0;password[strcspn(password,"\r\n")]=0;
    screen=LOGIN;focus=2;activate();if(screen!=HOME)goto fail;
    focus=1;activate();if(screen!=DOWNLOADS || !remote_more)goto fail;
    switch_card(1);if(remote_page!=1 || remote_more || remote_count!=1)goto fail;
    switch_card(-1);if(remote_page!=0)goto fail;
    for(remote_selected=0;remote_selected<remote_count;++remote_selected)
        if(!strcmp(remote_saves[remote_selected].filename,"DCVMU_TEST"))break;
    if(remote_selected>=remote_count)goto fail;
    remote_save_t original=remote_saves[remote_selected];
    activate();if(screen!=DESTINATION || !save_data)goto fail;
    for(target_index=0;target_index<card_count;++target_index)if(card_ids[target_index]==2)break;
    if(target_index>=card_count)goto fail;
    activate();if(screen!=INSTALL_CONFIRM || focus!=0 || target_exists)goto fail;
    /* Default confirmation must cancel without writing. */
    activate();if(screen!=DESTINATION)goto fail;
    activate();if(screen!=INSTALL_CONFIRM || target_exists)goto fail;
    focus=1;activate();if(screen!=INSTALLED)goto fail;
    printf("dcvmu: DOWNLOAD INSTALL AND READBACK PASS\n");
    maple_device_t *dev=maple_enum_dev(0,2);vmu_dir_t entry;
    if(find_target(dev,&entry)!=1 || entry.hdroff!=original.header_offset)goto fail;
    back();if(screen!=DOWNLOADS)goto fail;
    activate();if(screen!=DESTINATION)goto fail;
    target_index=1;activate();if(screen!=INSTALL_CONFIRM || !target_exists || focus!=0)goto fail;
    back();if(screen!=DESTINATION)goto fail;
    activate();focus=1;activate();if(screen!=INSTALLED)goto fail;
    printf("dcvmu: OVERWRITE CONFIRM AND HEADER OFFSET PASS\n");
    back();activate();target_index=1;
    strcpy(remote_saves[remote_selected].filename,"OTHER_SAVE");
    prepare_install();if(screen!=DESTINATION)goto fail;
    remote_saves[remote_selected]=original;
    printf("dcvmu: FULL VMU REFUSAL PASS\n");
    void *bad=NULL;remote_saves[remote_selected].sha256[0]=original.sha256[0]=='0'?'1':'0';
    if(service_download(token,&remote_saves[remote_selected],&bad)==0 || bad)goto fail;
    remote_saves[remote_selected]=original;
    printf("dcvmu: DOWNLOAD CHECKSUM REFUSAL PASS\n");
    back();back();if(screen!=HOME)goto fail;
    focus=0;activate();if(screen!=FILES)goto fail;
    back();if(screen!=HOME)goto fail;
    focus=4;activate();if(screen!=PUBLIC_SEARCH)goto fail;
    strcpy(browse_user,username);focus=2;activate();
    if(remote_count!=7 || remote_more)goto fail;
    back();if(screen!=PUBLIC_SEARCH)goto fail;
    back();if(screen!=HOME)goto fail;
    printf("dcvmu: DOWNLOAD NAVIGATION AND PUBLIC LIST PASS\n");
    sign_out();printf("dcvmu: DOWNLOAD SELF-TEST PASSED\n");return;
fail:printf("dcvmu: DOWNLOAD SELF-TEST FAILED screen=%d status=%s\n",screen,status_text);
}
#endif

#ifdef DCVMU_PUBLIC_TEST
#ifndef DCVMU_DOWNLOAD_TEST
#error "PUBLIC_TEST requires DOWNLOAD_TEST and the isolated HTTPS fixture"
#endif
#include "tests/public_browse.c"
#endif

#ifdef DCVMU_TOOLS_TEST
#ifndef DCVMU_DOWNLOAD_TEST
#error "TOOLS_TEST requires DOWNLOAD_TEST and the isolated HTTPS fixture"
#endif
#include "tests/vmu_tools.c"
#endif

int main(int argc,char **argv) {
    int quit=0,online=0;
    (void)argc;(void)argv;
    vid_set_mode(DM_640x480,PM_RGB565);bfont_set_encoding(BFONT_CODE_ISO8859_1);
    client_status("Starting DCVMU...");draw();
    if(service_net_init()==0) {online=1;client_status("Ready. HTTPS certificate checks enabled.");}
    canceled=0;
    printf("dcvmu: startup %s\n",online?"ready":"offline");
#ifdef DCVMU_AUTH_TEST
    char test_user[25]={0},test_token[96]={0};
    int existing=auth_load(test_user,test_token)==0;
    const char *expected="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ";
    if(existing) {
        if(strcmp(test_user,"vmuauth_test") || strcmp(test_token,expected))return 10;
        if(auth_forget()!=0 || auth_load(test_user,test_token)==0)return 11;
        printf("dcvmu: AUTH REBOOT AND FORGET PASS\n");
    } else {
        screen=FILES;scan();int before=save_count;
        if(auth_save("vmuauth_test",expected)!=0)return 12;
        if(auth_load(test_user,test_token)!=0 || strcmp(test_token,expected))return 13;
        scan();if(save_count!=before)return 14;
        printf("dcvmu: AUTH WRITE READ AND FILTER PASS\n");
    }
    memset(test_token,0,sizeof(test_token));screen=FILES;scan();
    client_status("VMU login persistence test passed.");
#else
    if(online && auth_load(username,token)==0) {
        screen=AUTO_LOGIN;
        client_status("Connecting securely to dcvmu.com...");draw();
        printf("dcvmu: attempting auto login\n");
        int restored=service_resume(token);
        if(restored==0){remembered=1;screen=HOME;focus=0;client_status("Restored saved VMU login.");}
        else {if(restored==-2)auth_forget();memset(token,0,sizeof(token));screen=LOGIN;
            client_status(restored==-2?"Saved login expired. Please log in.":"Could not restore login. Check connection.");}
    } else screen=LOGIN;
#endif
#ifdef DCVMU_SELF_TEST
    if(online)run_self_test();
#endif
#ifdef DCVMU_VMU_TEST
    /* Read-only emulator check: mount a populated A1 and an empty A2. */
    screen=FILES; scan();
    int total=save_count;
    if(card_count!=2 || total==0) return 1;
    switch_card(1);
    if(card_filter!=1 || save_count!=total) return 2;
    switch_card(1);
    if(card_filter!=2 || save_count!=0) return 3;
    switch_card(1);
    if(card_filter!=-1 || save_count!=total) return 4;
    switch_card(-1);
    if(card_filter!=2 || save_count!=0) return 5;
    switch_card(-1);
    if(card_filter!=1 || save_count!=total) return 6;
    printf("dcvmu: VMU SWITCH SELF-TEST PASS (%d saves on A1, empty A2)\n",total);
#endif
#if defined(DCVMU_DOWNLOAD_TEST) && !defined(DCVMU_PUBLIC_TEST) && !defined(DCVMU_TOOLS_TEST)
    if(online)run_download_test();
#endif
#ifdef DCVMU_PUBLIC_TEST
    if(online)run_public_test();
#endif
#ifdef DCVMU_TOOLS_TEST
    if(online)run_tools_test();
#endif
    while(!quit) {
        maple_device_t *dev=maple_enum_type(0,MAPLE_FUNC_CONTROLLER);
        cont_state_t *state=dev?maple_dev_status(dev):NULL;
        uint32_t pressed=state?state->buttons&~previous_buttons:0;
        if(state)previous_buttons=state->buttons;
        if(pressed&CONT_START)break;
        if(edit_field>=0) {
            if(pressed&CONT_DPAD_UP)osk_y=(osk_y+5)%6;
            if(pressed&CONT_DPAD_DOWN)osk_y=(osk_y+1)%6;
            if(pressed&CONT_DPAD_LEFT)osk_x=(osk_x+(int)strlen(keys[osk_y])-1)%(int)strlen(keys[osk_y]);
            if(pressed&CONT_DPAD_RIGHT)osk_x=(osk_x+1)%(int)strlen(keys[osk_y]);
            if(osk_x>=(int)strlen(keys[osk_y]))osk_x=0;
            if(pressed&CONT_A)edit_char(keys[osk_y][osk_x]);
            if(pressed&CONT_X)edit_char('\b');
            if(pressed&CONT_Y)edit_end(0);
            if(pressed&CONT_B)back();
            if(pressed)dirty=1;
        } else {
            if(pressed&CONT_DPAD_LEFT)switch_card(-1);
            if(pressed&CONT_DPAD_RIGHT)switch_card(1);
            if(pressed&CONT_DPAD_UP)move(-1);
            if(pressed&CONT_DPAD_DOWN)move(1);
            if((pressed&CONT_A)&&online)activate();
            if(pressed&CONT_B)back();
            if(pressed&CONT_X){if(screen==HOME)sign_out();else cloud_action();}
            if(pressed&CONT_Y) {if(screen==DOWNLOADS)refresh_downloads();else if(screen==ARCHIVES)refresh_archives();
                else if(screen==DESTINATION||screen==ARCHIVE_SOURCE||screen==RESTORE_DEST)destination_cards();else if(screen==FILES)scan();else if(screen==CONFLICT)upload("keep");}
        }
        dev=maple_enum_type(0,MAPLE_FUNC_KEYBOARD);
        if(dev) {
            int raw;
            while((raw=kbd_queue_pop(dev,0))!=KBD_QUEUE_END) {
                kbd_key_t key=raw&255;kbd_mods_t mods={.raw=(raw>>8)&255};kbd_leds_t leds={.raw=(raw>>16)&255};
                kbd_state_t *ks=maple_dev_status(dev);
                char ascii=ks?kbd_key_to_ascii(key,ks->region,mods,leds):0;
                if(edit_field>=0) {
                    if(key==KBD_KEY_ENTER||key==KBD_KEY_TAB)edit_end(0);
                    else if(key==KBD_KEY_ESCAPE)edit_end(1);
                    else if(key==KBD_KEY_BACKSPACE)edit_char('\b');
                    else edit_char(ascii);
                } else if(key==KBD_KEY_ESCAPE){if(screen==LOGIN||screen==HOME){quit=1;break;}else back();}
                else if(key==KBD_KEY_LEFT)switch_card(-1);
                else if(key==KBD_KEY_RIGHT)switch_card(1);
                else if(key==KBD_KEY_UP)move(-1);
                else if(key==KBD_KEY_DOWN||key==KBD_KEY_TAB)move(1);
                else if(key==KBD_KEY_ENTER&&online)activate();
                else if(key==KBD_KEY_BACKSPACE)back();
                else if(key==KBD_KEY_R) {
                    if(screen==DOWNLOADS)refresh_downloads();else if(screen==ARCHIVES)refresh_archives();
                    else if(screen==DESTINATION||screen==ARCHIVE_SOURCE||screen==RESTORE_DEST)destination_cards();else if(screen==FILES)scan();
                }
                else if(key==KBD_KEY_N)cloud_action();
                else if(key==KBD_KEY_L&&screen==HOME)sign_out();
                else if(key==KBD_KEY_K&&screen==CONFLICT)upload("keep");
            }
        }
        if(dirty)draw();
        thd_sleep(16);
    }
    if(online&&token[0]&&!remembered){canceled=0;service_logout(token);}
    memset(password,0,sizeof(password));memset(token,0,sizeof(token));memset(edit_backup,0,sizeof(edit_backup));
    free(target_old);free(save_data);free(image);if(online)service_net_shutdown();
    /* Returning from main tears down video. Keep the final frame displayed
       until the console is powered off/reset or the emulator is closed. */
    vid_waitvbl();
    for(int i=0;i<640*480;++i)vram_s[i]=PAPER;
    text(24,18,ORANGE,"DCVMU.com / Cloud Saves for Dreamcast");
    text(24,144,BLUE,"Thank you for using DCVMU!");
    text(24,192,INK,"See you next time at dcvmu.com.");
    text(24,288,INK,"You can now turn off your Dreamcast");
    text(24,320,INK,"or close the emulator.");
    printf("dcvmu: clean shutdown - thank-you screen\n");
    for(;;)thd_sleep(1000);
}
