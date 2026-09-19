/* Isolated local HTTPS fixture, synthetic VMUs only. */
static int tools_select(const char *filename) {
    for(remote_selected=0;remote_selected<remote_count;++remote_selected)
        if(!strcmp(remote_saves[remote_selected].filename,filename))return 0;
    return -1;
}
static void run_tools_test(void) {
    void *original=NULL,*readback=NULL;int original_size=0,readback_size=0;
    FILE *fixture=fopen("/rd/test-service.txt","r");
    if(!fixture)goto fail;
    fclose(fixture);
    strcpy(username,"reader");strcpy(password,"local-fixture-password");
    screen=LOGIN;focus=2;activate();if(screen!=HOME)goto fail;
    focus=1;activate();if(screen!=DOWNLOADS || remote_count!=2)goto fail;
    if(tools_select("TEST_SAVE")<0)goto fail;
    activate();if(screen!=DESTINATION)goto fail;
    for(target_index=0;target_index<card_count;++target_index)if(card_ids[target_index]==2)break;
    if(target_index>=card_count)goto fail;
    activate();if(screen!=INSTALL_CONFIRM || target_exists)goto fail;
    focus=1;activate();if(screen!=INSTALLED)goto fail;
    maple_device_t *dev=maple_enum_dev(0,2);vmu_dir_t entry;
    if(find_target(dev,&entry)!=1 || entry.hdroff!=1 ||
       vmufs_read(dev,"TEST_SAVE",&original,&original_size)<0)goto fail;
    printf("dcvmu: IMPORTED SAVE INSTALL AND HEADER OFFSET PASS\n");
    back();if(screen!=DOWNLOADS || tools_select(DCVMU_ICON_FILE)<0)goto fail;
    if(!remote_icons[remote_selected].valid)goto fail;
    activate();if(screen!=DESTINATION || remote_saves[remote_selected].size!=1024)goto fail;
    for(target_index=0;target_index<card_count;++target_index)if(card_ids[target_index]==2)break;
    activate();if(screen!=INSTALL_CONFIRM || target_exists || focus!=0)goto fail;
    activate();if(screen!=DESTINATION)goto fail;
    if(find_target(dev,&entry)!=0)goto fail;
    printf("dcvmu: CUSTOM ICON CANCEL LEAVES CARD UNCHANGED PASS\n");
    activate();focus=1;activate();if(screen!=INSTALLED)goto fail;
    if(find_target(dev,&entry)!=1 || entry.filetype!=0x33 || entry.hdroff!=0 || entry.filesize!=2)goto fail;
    if(vmufs_read(dev,DCVMU_ICON_FILE,&readback,&readback_size)<0 || readback_size!=1024 ||
       memcmp(readback,save_data,1024))goto fail;
    static const unsigned char secret[16]={0xda,0x69,0xd0,0xda,0xc7,0x4e,0xf8,0x36,0x18,0x92,0x79,0x68,0x2d,0xb5,0x30,0x86};
    if(memcmp((unsigned char *)readback+704,secret,16))goto fail;
    free(readback);readback=NULL;
    printf("dcvmu: CUSTOM ICON DOWNLOAD INSTALL AND BIOS BYTES PASS\n");
    back();activate();target_index=1;activate();
    if(screen!=INSTALL_CONFIRM || !target_exists || focus!=0)goto fail;
    focus=1;activate();if(screen!=INSTALLED)goto fail;
    if(vmufs_read(dev,"TEST_SAVE",&readback,&readback_size)<0 ||
       readback_size!=original_size || memcmp(readback,original,original_size))goto fail;
    free(readback);readback=NULL;free(original);original=NULL;
    printf("dcvmu: ICON REPLACEMENT PRESERVES GAME SAVE PASS\n");
    back();back();focus=0;activate();
    if(screen!=FILES || save_count!=1 || strcmp(saves[0].filename,"TEST_SAVE"))goto fail;
    char saved_user[25],saved_token[96];
    if(auth_load(saved_user,saved_token)<0 || strcmp(saved_user,"reader"))goto fail;
    memset(saved_token,0,sizeof(saved_token));
    printf("dcvmu: ICON AND AUTH UPLOAD EXCLUSION PASS\n");
    printf("dcvmu: VMU TOOLS SELF-TEST PASSED\n");return;
fail:
    free(original);free(readback);
    printf("dcvmu: VMU TOOLS SELF-TEST FAILED screen=%d count=%d status=%s\n",screen,remote_count,status_text);
}
