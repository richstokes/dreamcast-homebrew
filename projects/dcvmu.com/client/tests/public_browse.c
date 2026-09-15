/* Included only in the isolated PUBLIC_TEST build; drives real UI handlers. */
static void run_public_test(void) {
    FILE *fixture=fopen("/rd/test-service.txt","r");
    if(!fixture){printf("dcvmu: PUBLIC BROWSE SELF-TEST FAILED: local fixture required\n");return;}
    fclose(fixture);
    strcpy(username,"ReAdEr");strcpy(password,"local-fixture-password");
    screen=LOGIN;focus=2;activate();if(screen!=HOME)goto fail;
    move(-1);if(focus!=3)goto fail;
    move(-1);activate();if(screen!=PUBLIC_SEARCH || focus!=0)goto fail;
    focus=2;activate();if(screen!=PUBLIC_SEARCH)goto fail; /* Empty owner. */
    focus=0;activate();if(edit_field!=0)goto fail;
    for(const char *p="ShArEr";*p;++p)edit_char(*p);
    edit_end(0);focus=1;activate();edit_char('x');back();
    if(browse_game[0] || edit_field!=-1)goto fail; /* Canceled edit. */
    focus=2;activate();
    if(screen!=DOWNLOADS || remote_count!=7 || !remote_more || remote_page!=0)goto fail;
    int first=remote_saves[0].id;
    move(1);
    /* The fixture rejects the first next-page request to test recovery. */
    switch_card(1);
    if(remote_page!=0 || remote_count!=7 || remote_saves[0].id!=first || remote_selected!=1)goto fail;
    switch_card(1);
    if(remote_page!=1 || remote_count!=1 || remote_more)goto fail;
    switch_card(1);if(remote_page!=1)goto fail;
    switch_card(-1);if(remote_page!=0 || remote_saves[0].id!=first)goto fail;
    printf("dcvmu: PUBLIC PAGINATION AND FAILED PAGE RECOVERY PASS\n");
    back();if(screen!=PUBLIC_SEARCH || strcmp(browse_user,"ShArEr"))goto fail;
    strcpy(browse_game,"stone");focus=2;activate();
    if(screen!=DOWNLOADS || remote_count!=3 || remote_more)goto fail;
    activate();if(screen!=DESTINATION || !save_data)goto fail;
    for(target_index=0;target_index<card_count;++target_index)if(card_ids[target_index]==2)break;
    if(target_index>=card_count)goto fail;
    activate();if(screen!=INSTALL_CONFIRM || focus!=0 || target_exists)goto fail;
    activate();if(screen!=DESTINATION)goto fail; /* Default cancel. */
    activate();focus=1;activate();if(screen!=INSTALLED)goto fail;
    back();if(screen!=DOWNLOADS)goto fail;
    printf("dcvmu: OTHER USER PUBLIC DOWNLOAD INSTALL AND READBACK PASS\n");
    back();strcpy(browse_game,"Missing game");focus=2;activate();
    if(screen!=DOWNLOADS || remote_count || remote_more)goto fail;
    back();browse_game[0]=0;strcpy(browse_user,"nobody");focus=2;activate();
    if(screen!=DOWNLOADS || remote_count || remote_more)goto fail;
    back();strcpy(browse_user,"legacy");focus=2;activate();
    if(screen!=PUBLIC_SEARCH)goto fail; /* Server ignored the owner filter. */
    strcpy(browse_user,"reader");focus=2;activate();
    if(screen!=DOWNLOADS || remote_count)goto fail; /* Own private save excluded. */
    back();back();if(screen!=HOME || focus!=2)goto fail;
    focus=1;activate();if(screen!=DOWNLOADS || remote_count!=1)goto fail;
    back();focus=2;activate();strcpy(browse_user,"sharer");strcpy(browse_game,"stone");focus=2;activate();
    refresh_downloads();if(screen!=DOWNLOADS || remote_page || remote_count!=3)goto fail;
    printf("dcvmu: PUBLIC SEARCH EMPTY PRIVACY AND REFRESH PASS\n");
    back();back();focus=0;activate();
    for(selected=0;selected<save_count;++selected)if(!strcmp(saves[selected].filename,"SAVE_4"))break;
    if(selected==save_count)goto fail;
    choose_save();if(screen!=DETAILS || focus!=0)goto fail;
    activate();if(edit_field!=0)goto fail;
    while(save_name[0])edit_char('\b');
    for(const char *p="Before final boss";*p;++p)edit_char(*p);
    edit_end(0);move(1);if(focus!=2)goto fail; /* Fixed game is skipped. */
    focus=4;activate();if(screen!=SUCCESS)goto fail;
    choose_save();upload("ask");
    if(screen!=DOWNLOADS || !remote_matching || remote_count!=1)goto fail;
    remote_save_t original=remote_saves[0];
    if(strcmp(original.name,"Before final boss"))goto fail;
    /* A concurrent metadata rename invalidates the pending replacement. */
    activate();if(screen!=CONFLICT || replacement_id!=original.id)goto fail;
    if(service_rename(token,&original,"Renamed elsewhere")<0)goto fail;
    upload("replace");if(screen!=CONFLICT)goto fail;
    back();refresh_downloads();activate();
    if(revision!=original.revision+1)goto fail;
    upload("replace");if(screen!=SUCCESS)goto fail;
    remote_save_t matches[7];int count,more;
    if(service_matches(token,"SAVE_4",0,matches,&count,&more)<0 || count!=1 || more ||
       matches[0].id!=original.id || strcmp(matches[0].name,"Renamed elsewhere") ||
       strcmp(matches[0].sha256,original.sha256))goto fail;
    printf("dcvmu: CUSTOM TITLE AND STALE ID REPLACEMENT PASS\n");
    choose_save();upload("ask");if(screen!=DOWNLOADS || !remote_matching)goto fail;
    cloud_action();if(screen!=SUCCESS)goto fail; /* Keep a new copy. */
    screen=HOME;focus=1;activate();
    if(screen!=DOWNLOADS || remote_matching || remote_count!=3)goto fail;
    for(remote_selected=0;remote_selected<remote_count;++remote_selected)
        if(remote_saves[remote_selected].id==original.id)break;
    if(remote_selected==remote_count)goto fail;
    cloud_action();if(screen!=RENAME || focus!=0)goto fail;
    activate();edit_char('x');back();if(strcmp(cloud_title,"Renamed elsewhere"))goto fail;
    activate();while(cloud_title[0])edit_char('\b');
    for(const char *p="Client renamed title";*p;++p)edit_char(*p);
    edit_end(0);move(1);activate();if(screen!=DOWNLOADS)goto fail;
    if(service_matches(token,"SAVE_4",0,matches,&count,&more)<0 || count!=2 || more)goto fail;
    int renamed=0;for(int i=0;i<count;++i)if(matches[i].id==original.id) {
        if(strcmp(matches[i].name,"Client renamed title") || strcmp(matches[i].sha256,original.sha256))goto fail;
        renamed=1;
    }
    if(!renamed)goto fail;
    printf("dcvmu: KEEP BOTH AND CLOUD RENAME WITHOUT BINARY CHANGES PASS\n");
    int was_ppp=net_default_dev && !strcmp(net_default_dev->name,"ppp");
    service_net_shutdown();service_net_shutdown();
    if(was_ppp && net_default_dev)goto fail;
    printf("dcvmu: NETWORK SHUTDOWN PASS\n");
    printf("dcvmu: PUBLIC BROWSE SELF-TEST PASSED\n");
    return;
fail:
    printf("dcvmu: PUBLIC BROWSE SELF-TEST FAILED screen=%d page=%d count=%d status=%s\n",
           screen,remote_page,remote_count,status_text);
}
