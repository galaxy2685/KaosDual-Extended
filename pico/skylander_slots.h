#pragma once
#ifndef SKYLANDER_SLOTS_H
#define SKYLANDER_SLOTS_H

#include <stdint.h>
#include <stdbool.h>
#include "kaos_protocol.h"

/* The portal protocol and status report can represent four physical slots.
 * KaosDual currently exposes only slots 0-2 from the ESP32, leaving slot 3
 * unused until a future P4 feature is deliberately enabled. */
#define MAX_SLOTS 4

typedef struct {
    bool    loaded;
    bool    active;
    bool    dirty;
    uint8_t data[SKYLANDER_DUMP_SIZE];
    /* Raw dump at load time.  Used only to audit write-back fidelity. */
    uint8_t original_data[SKYLANDER_DUMP_SIZE];
    bool    game_wrote_block[SKYLANDER_DUMP_SIZE / 16];
    uint8_t uid[4];
} slot_t;

extern slot_t g_slots[MAX_SLOTS];

void    slots_load(uint8_t slot, const uint8_t *dump_1024);
void    slots_unload(uint8_t slot);
uint8_t slots_portal_status(void);
uint8_t *slots_get_block(uint8_t slot, uint8_t block);
void    slots_write_block(uint8_t slot, uint8_t block, const uint8_t *data);

#endif
