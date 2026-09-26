/* Whole-card VMU archives: raw block copies with the DCVMU login save removed. */
#include "client.h"
#include <kos.h>
#include <dc/maple/vmu.h>
#include <dc/vmufs.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define BLOCKS (ARCHIVE_IMAGE_SIZE/512)
#define FAT_FREE 0xFFFC

typedef struct { unsigned fat, dir_end, dir_size, user; } layout_t;
static unsigned rd16(const unsigned char *p) {return p[0]|((unsigned)p[1]<<8);}
static void wr16(unsigned char *p,unsigned v) {p[0]=v&255;p[1]=(v>>8)&255;}

/* Validate the root block of a standard 128 KiB card (dc/vmufs.h vmu_root_t). */
static int layout(const unsigned char *image,layout_t *out) {
    const unsigned char *root=image+255*512;
    for(int i=0;i<16;++i)if(root[i]!=0x55)return -1;
    unsigned fat_size=rd16(root+0x48);
    out->fat=rd16(root+0x46);out->dir_end=rd16(root+0x4a);out->dir_size=rd16(root+0x4c);
    out->user=rd16(root+0x50);if(!out->user)out->user=200;
    if(fat_size!=1 || out->fat>=255 || out->dir_size<1 || out->dir_size>16 ||
       out->dir_end<out->dir_size || out->dir_end>=255)return -1;
    unsigned dir_start=out->dir_end-out->dir_size+1;
    if(out->fat>=dir_start && out->fat<=out->dir_end)return -1;
    if(out->user>255 || out->fat<out->user || dir_start<out->user)return -1;
    return 0;
}

int archive_scrub(unsigned char *image,int *files) {
    layout_t l;
    if(layout(image,&l)<0)return -1;
    unsigned char *fat=image+l.fat*512;
    static unsigned char owned[BLOCKS];
    memset(owned,0,sizeof(owned));
    *files=0;
    for(unsigned d=0;d<l.dir_size;++d)for(unsigned e=0;e<16;++e) {
        unsigned char *entry=image+(l.dir_end-d)*512+e*32;
        if(!entry[0])continue;
        char name[13];memcpy(name,entry+4,12);name[12]=0;
        for(int j=11;j>=0 && (name[j]==' '||name[j]==0);--j)name[j]=0;
        unsigned current=rd16(entry+2),count=rd16(entry+24);
        static unsigned char visited[BLOCKS];static unsigned short chain[BLOCKS];
        memset(visited,0,sizeof(visited));
        int n=0,login=auth_is_save(name,NULL,0);
        for(unsigned i=0;i<count && i<=241;++i) {
            if(current>=l.user || visited[current])break;
            visited[current]=1;chain[n++]=(unsigned short)current;
            if(auth_is_save(name,image+current*512,512))login=1;
            current=rd16(fat+current*2);
        }
        if(login) {
            for(int i=0;i<n;++i){memset(image+chain[i]*512,0,512);wr16(fat+chain[i]*2,FAT_FREE);}
            memset(entry,0,32);
            continue;
        }
        for(int i=0;i<n;++i)owned[chain[i]]=1;
        ++*files;
    }
    /* Free and leaked user blocks are zeroed so deleted tokens cannot linger. */
    for(unsigned b=0;b<l.user;++b)if(!owned[b])memset(image+b*512,0,512);
    return auth_is_save("",image,ARCHIVE_IMAGE_SIZE)?-1:0;
}

int archive_read(maple_device_t *dev,unsigned char *image) {
    int result=0;
    if(vmufs_mutex_lock()<0)return -1;
    for(int b=0;b<BLOCKS;++b) {
        if(vmu_block_read(dev,(uint16_t)b,image+b*512)!=MAPLE_EOK){result=-1;break;}
        if((b&31)==31)archive_progress("Reading VMU",b+1,BLOCKS);
    }
    vmufs_mutex_unlock();
    if(result<0)memset(image,0,ARCHIVE_IMAGE_SIZE);
    return result;
}

int archive_write(maple_device_t *dev,const unsigned char *image,unsigned char *readback) {
    int result=0;
    if(vmufs_mutex_lock()<0)return -1;
    for(int b=0;b<BLOCKS && result==0;++b) {
        if(vmu_block_write(dev,(uint16_t)b,image+b*512)!=MAPLE_EOK)result=-1;
        if((b&15)==15)archive_progress("Writing VMU",b+1,BLOCKS);
    }
    for(int b=0;b<BLOCKS && result==0;++b) {
        if(vmu_block_read(dev,(uint16_t)b,readback+b*512)!=MAPLE_EOK ||
           memcmp(readback+b*512,image+b*512,512))result=-2;
        if((b&31)==31)archive_progress("Verifying VMU",b+1,BLOCKS);
    }
    vmufs_mutex_unlock();
    return result;
}

int archive_card_files(maple_device_t *dev) {
    vmu_dir_t *entries=NULL;int count=0,files=0;
    if(vmufs_readdir(dev,&entries,&count)<0)return -1;
    for(int i=0;i<count;++i) {
        char name[13];memcpy(name,entries[i].filename,12);name[12]=0;
        for(int j=11;j>=0 && (name[j]==' '||name[j]==0);--j)name[j]=0;
        if(entries[i].filetype && !auth_is_save(name,NULL,0))++files;
    }
    free(entries);
    return files;
}
