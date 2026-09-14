#include <kos.h>
#include <dc/vmufs.h>
#include <dc/vmu_pkg.h>
#include <dc/biosfont.h>
#include <dc/video.h>
KOS_INIT_FLAGS(INIT_DEFAULT);
#ifndef DCVMU_FIXTURE_BYTES
#define DCVMU_FIXTURE_BYTES 8192
#endif
int main(void) {
    maple_device_t *vmu=maple_enum_type(0,MAPLE_FUNC_MEMCARD);
    vmu_pkg_t pkg={0}; uint8_t *data=NULL; int size=0;
    static uint8_t payload[DCVMU_FIXTURE_BYTES];
    vid_set_mode(DM_640x480,PM_RGB565);
    if(!vmu) {printf("fixture: no VMU\n");return 1;}
    for(int i=0;i<DCVMU_FIXTURE_BYTES;++i)payload[i]=(uint8_t)i;
    strcpy(pkg.desc_short,"DCVMU TEST");strcpy(pkg.desc_long,"DCVMU Test Game");strcpy(pkg.app_id,"DCVMU_TEST");
    pkg.data=payload;pkg.data_len=sizeof(payload);
    if(vmu_pkg_build(&pkg,&data,&size)<0)return 2;
    int result=vmufs_write(vmu,"DCVMU_TEST",data,size,VMUFS_OVERWRITE);
    free(data);printf("fixture: wrote synthetic VMU save result=%d size=%d\n",result,size);
    bfont_draw_str(vram_s+640*100+24,640,0,"DCVMU test fixture ready");
    for(;;)thd_sleep(100);
}
