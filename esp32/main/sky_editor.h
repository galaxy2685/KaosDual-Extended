#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "library.h"

/* Inspector/editor result. No member points into the source dump. */
typedef struct {
    uint16_t stored[4];
    uint16_t calculated[4];
    bool valid[4];
    bool uninitialised;
} sky_editor_area_checks_t;

/* Read-only progression components in the normal-character save layout.
 * Their interpretation as a universal level is intentionally not assumed. */
typedef struct {
    uint16_t component_1;
    uint16_t component_2;
    uint32_t component_3;
    uint32_t total;
} sky_editor_progression_t;

typedef enum {
    SKY_EDITOR_UNSUPPORTED = 0,
    SKY_EDITOR_NORMAL_CHARACTER,
    SKY_EDITOR_TRAP,
    SKY_EDITOR_VEHICLE,
    SKY_EDITOR_CREATION_CRYSTAL,
    SKY_EDITOR_ITEM,
    SKY_EDITOR_SIDEKICK,
    SKY_EDITOR_SENSEI,
    SKY_EDITOR_UNKNOWN,
} sky_editor_figure_class_t;

typedef enum {
    SKY_GENERATION_UNKNOWN = 0,
    SKY_GENERATION_SSA,
    SKY_GENERATION_GIANTS,
    SKY_GENERATION_SWAP_FORCE,
    SKY_GENERATION_TRAP_TEAM,
    SKY_GENERATION_SUPERCHARGERS,
    SKY_GENERATION_IMAGINATORS,
} sky_editor_generation_t;

typedef struct {
    bool supported;
    bool encrypted;
    bool header_valid;
    uint16_t header_stored;
    uint16_t header_calculated;
    bool area_a_valid;
    bool area_b_valid;
    uint16_t character_id;
    sky_editor_figure_class_t figure_class;
    sky_editor_generation_t generation;
    uint8_t area_a_counter;
    uint8_t area_b_counter;
    uint16_t area_a_gold;
    uint16_t area_b_gold;
    uint16_t selected_gold;
    char selected_area;
    char state[24];
    char warning[160];
    sky_editor_area_checks_t area_a_checks;
    sky_editor_area_checks_t area_b_checks;
    sky_editor_progression_t area_a_progression;
    sky_editor_progression_t area_b_progression;
    uint8_t derived_level;
    bool derived_level_provisional;
    char error[128];
} sky_editor_info_t;

/* Metadata is a first gate only; the decoded dump structure is authoritative. */
sky_editor_figure_class_t sky_editor_entry_class(const library_entry_t *entry);
const char *sky_editor_figure_class_name(sky_editor_figure_class_t figure_class);
const char *sky_editor_generation_name(sky_editor_generation_t generation);
bool sky_editor_entry_supported(const library_entry_t *entry);

/* Reads and validates a file without changing its bytes or metadata. */
esp_err_t sky_editor_inspect_file(const library_entry_t *entry,
                                  sky_editor_info_t *result);

/* Gold and level editing are available only when inspection has a
 * checksum-validated active normal-character save area. */
bool sky_editor_gold_editable(const sky_editor_info_t *info);
bool sky_editor_level_editable(const sky_editor_info_t *info);

/* The edit path is transactional: it verifies filename.sky.tmp before the
 * original becomes filename.sky.bak, then reloads the installed .sky file
 * and compares its gold value with the requested value. */
esp_err_t sky_editor_save_gold(const library_entry_t *entry, uint16_t gold,
                               sky_editor_info_t *result);
/* Sets the active save area's cumulative progression to the exact start of
 * the requested level (1 through 20), then uses the same transactional
 * temporary-file, backup and installed-file verification as Gold editing. */
esp_err_t sky_editor_save_level(const library_entry_t *entry, uint8_t level,
                                sky_editor_info_t *result);
bool sky_editor_backup_exists(const library_entry_t *entry);
esp_err_t sky_editor_restore_backup(const library_entry_t *entry,
                                    sky_editor_info_t *result);
