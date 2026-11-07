/* Fixed inputs and animation time make both render builds visit identical
   camera poses. Run serially in the same Flycast configuration. */
extern uint64_t renderer_cpu_us;
static int run_render_benchmark(void) {
    const unsigned warmup=12,samples=48;
    uint64_t all_frames=0,all_render=0,all_camera=0;
    unsigned measured=0;
#ifdef IE_REFERENCE_RENDER
    const char *mode="reference";
#else
    const char *mode="sh4zam";
#endif
    printf("[bench] BEGIN mode=%s\n",mode);
    for(unsigned stop=0;stop<destination_count;stop++) {
        travel_to(stop);
        game.fade=0;game.arrival=0;game.debug=false;
        for(unsigned view=0;view<4;view++) {
            game.camera.yaw=destinations[stop].yaw+view*1.570796327f;
            game.camera.target=add(game.player.position,v3(0,6.7f,0));
            uint64_t frame_us=0,render_us=0,camera_us=0;
            for(unsigned i=0;i<warmup+samples;i++) {
                game.time=21; /* Freeze waves, sky, HUD and character pose. */
                uint64_t begin=timer_us_gettime64();
                update_camera(1.f/60,0,0,false);
                uint64_t camera_done=timer_us_gettime64();
                audio_update(1.f/60);
                renderer_draw();
                uint64_t elapsed=timer_us_gettime64()-begin;
                if(i>=warmup) {
                    frame_us+=elapsed;render_us+=renderer_cpu_us;
                    camera_us+=camera_done-begin;
                }
                thd_pass();
            }
            printf("[bench] stop=%u view=%u frames=%u frame_us=%llu render_us=%llu camera_us=%llu tris=%u batches=%u\n",
                   stop,view,samples,(unsigned long long)frame_us,
                   (unsigned long long)render_us,(unsigned long long)camera_us,
                   game.triangles,game.visible_meshes);
            all_frames+=frame_us;all_render+=render_us;all_camera+=camera_us;
            measured+=samples;
        }
    }
    printf("[bench] DONE mode=%s frames=%u frame_us=%llu render_us=%llu camera_us=%llu\n",
           mode,measured,(unsigned long long)all_frames,(unsigned long long)all_render,
           (unsigned long long)all_camera);
    return 0;
}
