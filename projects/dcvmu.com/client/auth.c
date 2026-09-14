#include <kos.h>
#include <dc/vmufs.h>
#include <dc/vmu_pkg.h>
#include <strings.h>
#include "client.h"
#define AUTH_FILE "DCVMU_AUTH"
typedef struct { char magic[16], username[25], token[96]; } auth_record;
static int auth_port=-1, auth_unit=-1;
int auth_is_save(const char *filename,const void *data,size_t size) {
    const unsigned char *b=data;
    if(!strcasecmp(filename,AUTH_FILE))return 1;
    if(size>=64 && !memcmp(b+48,AUTH_FILE,10))return 1;
    for(size_t i=0;i+13<=size;++i)if(!memcmp(b+i,"DCVMU-AUTH-V1",13))return 1;
    return 0;
}
int auth_load(char *username,char *token) {
    for(int n=0;;++n) {
        maple_device_t *dev=maple_enum_type(n,MAPLE_FUNC_MEMCARD);
        void *raw=NULL;int size=0;vmu_pkg_t pkg;
        if(!dev)break;
        if(vmufs_read(dev,AUTH_FILE,&raw,&size)<0)continue;
        int valid=0;
        if(size>0 && vmu_pkg_parse(raw,(size_t)size,&pkg)==0 && pkg.data_len==sizeof(auth_record)) {
            auth_record record;memcpy(&record,pkg.data,sizeof(record));
            if(!memcmp(record.magic,"DCVMU-AUTH-V1",13) && record.username[24]==0 && record.token[95]==0 &&
               strlen(record.username)>0 && strlen(record.token)==43 && strspn(record.token,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")==43) {
                strcpy(username,record.username);strcpy(token,record.token);
                auth_port=dev->port;auth_unit=dev->unit;valid=1;
            }
            memset(&record,0,sizeof(record));
        }
        memset(raw,0,size);free(raw);
        if(valid)return 0;
    }
    return -1;
}
int auth_save(const char *username,const char *token) {
    auth_record record={0};vmu_pkg_t pkg={0};uint8_t *raw=NULL;int size=0,result=-1;
    strcpy(record.magic,"DCVMU-AUTH-V1");snprintf(record.username,sizeof(record.username),"%s",username);
    snprintf(record.token,sizeof(record.token),"%s",token);
    strcpy(pkg.desc_short,"DCVMU Login");strcpy(pkg.desc_long,"Private login - do not share");strcpy(pkg.app_id,AUTH_FILE);
    pkg.data=(uint8_t *)&record;pkg.data_len=sizeof(record);
    if(vmu_pkg_build(&pkg,&raw,&size)<0)goto done;
    for(int n=0;;++n) {
        maple_device_t *dev=maple_enum_type(n,MAPLE_FUNC_MEMCARD);
        if(!dev)break;
        if(auth_port>=0 && (dev->port!=auth_port || dev->unit!=auth_unit))continue;
        /* Only overwrite the previously validated login save. */
        int flags=dev->port==auth_port && dev->unit==auth_unit?VMUFS_OVERWRITE:0;
        if(vmufs_write(dev,AUTH_FILE,raw,size,flags)==0) {
            auth_port=dev->port;auth_unit=dev->unit;result=0;break;
        }
    }
    memset(raw,0,size);free(raw);
done:
    memset(&record,0,sizeof(record));return result;
}
int auth_forget(void) {
    if(auth_port<0)return 0;
    maple_device_t *dev=maple_enum_dev(auth_port,auth_unit);
    if(!dev || !(dev->info.functions&MAPLE_FUNC_MEMCARD))return -1;
    int result=vmufs_delete(dev,AUTH_FILE);
    if(result==0){auth_port=-1;auth_unit=-1;}
    return result;
}
