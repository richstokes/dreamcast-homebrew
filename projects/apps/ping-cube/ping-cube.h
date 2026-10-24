#ifndef PING_CUBE_H
#define PING_CUBE_H

#include <stdint.h>

/* VMU LCD: latency history for the graph, newest last. -1 marks a timeout. */
void vmu_graph_reset(void);
void vmu_graph_push(int32_t latency_ms);

/* Compose the VMU screen and send it to every attached VMU when it changed.
   floor_ms is the smallest full-scale value of the graph. */
void vmu_display_draw(const char *title, const char *headline,
                      const char *unit, int floor_ms, uint64_t now_ms);

/* Blank every VMU screen (on exit). */
void vmu_display_clear(void);

#endif
