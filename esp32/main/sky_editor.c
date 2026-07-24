#include "sky_editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "mbedtls/aes.h"
#include "mbedtls/md5.h"

#define SKY_DUMP_SIZE 1024
#define AREA_A_BASE 0x080
#define AREA_B_BASE 0x240

/* Cumulative progression required for levels 1 through 20. */
static const uint32_t k_level_starts[] = {
    0, 1000, 2200, 3800, 6000, 9000, 13000, 18200, 24800, 33000,
    42700, 53900, 66600, 80800, 96500, 113700, 132400, 152600,
    174300, 197500,
};

/*
 * This is the independently reproduced on-dump key material used by normal
 * Skylanders save sectors: header bytes 0x00..0x1f, absolute block number,
 * then this 53-byte format constant.  It is deliberately local to the
 * read-only editor and is not used by portal emulation or writeback.
 */
static const uint8_t k_editor_key_magic[] =
    " Copyright (C) 2010 Activision. All Rights Reserved. ";

static void set_error(sky_editor_info_t *result, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(result->error, sizeof(result->error), format, args);
    va_end(args);
}

static bool has_sky_extension(const char *path);

static bool starts_with_root(const char *path, const char *root) {
    size_t length = strlen(root);
    return strncmp(path, root, length) == 0 && path[length] == '/';
}

static bool entry_path_is_approved(const library_entry_t *entry) {
    if (!entry || strstr(entry->path, "..") || !has_sky_extension(entry->path)) return false;
    if (strcasecmp(entry->source, "factory") == 0)
        return starts_with_root(entry->path, LIBRARY_ROOT);
    if (strcasecmp(entry->source, "user") == 0)
        return starts_with_root(entry->path, LIBRARY_USER_ROOT);
    return false;
}

static bool has_sky_extension(const char *path) {
    size_t length = strlen(path);
    /* .sky.tmp is used only by this module while it verifies an edit before
     * installing it. Library records themselves always use .sky. */
    return (length > 4 && strcasecmp(path + length - 4, ".sky") == 0) ||
           (length > 8 && strcasecmp(path + length - 8, ".sky.tmp") == 0);
}

static uint16_t read_le16(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void write_le16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static uint32_t read_le24(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16);
}

static void write_le24(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
}

static sky_editor_progression_t inspect_progression(
    const uint8_t dump[SKY_DUMP_SIZE], size_t base) {
    sky_editor_progression_t progression = {
        .component_1 = read_le16(&dump[base]),
        .component_2 = read_le16(&dump[base + 0x93]),
        .component_3 = read_le24(&dump[base + 0x98]),
    };
    progression.total = (uint32_t)progression.component_1 +
                        (uint32_t)progression.component_2 +
                        progression.component_3;
    return progression;
}

/* Cumulative-progression boundaries for levels 1 through 20. */
static uint8_t derived_level_from_progression(uint32_t total) {
    uint8_t level = 1;
    for (size_t i = 1; i < sizeof(k_level_starts) / sizeof(k_level_starts[0]); i++) {
        if (total < k_level_starts[i]) break;
        level = (uint8_t)(i + 1);
    }
    return level;
}

/* This matches the on-dump representation used by the original SkyEditGUI:
 * each component grows only after the previous component's known maximum.
 * The requested level is stored at its exact table-start progression value. */
static bool level_progression(uint8_t level, sky_editor_progression_t *result) {
    if (!result || level == 0 || level > sizeof(k_level_starts) / sizeof(k_level_starts[0]))
        return false;
    uint32_t total = k_level_starts[level - 1];
    memset(result, 0, sizeof(*result));
    if (total <= 33000) {
        result->component_1 = (uint16_t)total;
    } else if (total <= 96500) {
        result->component_1 = 33000;
        result->component_2 = (uint16_t)(total - 33000);
    } else {
        result->component_1 = 33000;
        result->component_2 = 63500;
        result->component_3 = total - 96500;
    }
    result->total = total;
    return true;
}

static void write_progression(uint8_t dump[SKY_DUMP_SIZE], size_t base,
                              const sky_editor_progression_t *progression) {
    write_le16(&dump[base], progression->component_1);
    write_le16(&dump[base + 0x93], progression->component_2);
    write_le24(&dump[base + 0x98], progression->component_3);
}

/* CRC-16/CCITT-FALSE: init FFFF, polynomial 1021, MSB first, no final xor. */
static uint16_t crc16_update(uint16_t crc, uint8_t value) {
    crc ^= (uint16_t)value << 8;
    for (int bit = 0; bit < 8; bit++)
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                               : (uint16_t)(crc << 1);
    return crc;
}

static uint16_t crc16_bytes(uint16_t crc, const uint8_t *bytes, size_t count) {
    for (size_t i = 0; i < count; i++) crc = crc16_update(crc, bytes[i]);
    return crc;
}

static bool inspect_header_crc(const uint8_t dump[SKY_DUMP_SIZE],
                               sky_editor_info_t *result) {
    result->header_stored = read_le16(&dump[0x1e]);
    result->header_calculated = crc16_bytes(0xffff, dump, 0x1e);
    return result->header_calculated == result->header_stored;
}

static void inspect_area_checks(const uint8_t dump[SKY_DUMP_SIZE], size_t base,
                                sky_editor_area_checks_t *checks) {
    uint16_t crc;
    memset(checks, 0, sizeof(*checks));

    /* Type 1: progress header plus the fixed normal-character selector. */
    crc = crc16_bytes(0xffff, &dump[base], 0x0e);
    crc = crc16_update(crc, 0x05);
    crc = crc16_update(crc, 0x00);
    checks->calculated[0] = crc;
    checks->stored[0] = read_le16(&dump[base + 0x0e]);
    checks->valid[0] = checks->calculated[0] == checks->stored[0];

    /* Type 2: two protected regions. */
    crc = crc16_bytes(0xffff, &dump[base + 0x10], 0x20);
    crc = crc16_bytes(crc, &dump[base + 0x40], 0x10);
    checks->calculated[1] = crc;
    checks->stored[1] = read_le16(&dump[base + 0x0c]);
    checks->valid[1] = checks->calculated[1] == checks->stored[1];

    /* Type 3: two regions followed by the specified zero-filled tail. */
    crc = crc16_bytes(0xffff, &dump[base + 0x50], 0x20);
    crc = crc16_bytes(crc, &dump[base + 0x80], 0x10);
    for (int i = 0; i < 224; i++) crc = crc16_update(crc, 0);
    checks->calculated[2] = crc;
    checks->stored[2] = read_le16(&dump[base + 0x0a]);
    checks->valid[2] = checks->calculated[2] == checks->stored[2];

    /* Type 4 covers two regions; its first two input bytes are normalised. */
    crc = 0xffff;
    crc = crc16_update(crc, 0x06);
    crc = crc16_update(crc, 0x01);
    crc = crc16_bytes(crc, &dump[base + 0x92], 0x1e);
    crc = crc16_bytes(crc, &dump[base + 0xc0], 0x20);
    checks->calculated[3] = crc;
    checks->stored[3] = read_le16(&dump[base + 0x90]);
    checks->valid[3] = checks->calculated[3] == checks->stored[3];

    /* Fresh KaosDual templates deliberately have all normal-save markers
     * cleared. MIFARE trailer bytes in the surrounding sectors are ignored. */
    checks->uninitialised = true;
    for (int i = 0; i < 4; i++)
        if (checks->stored[i] != 0) checks->uninitialised = false;
}

static bool all_checks_valid(const sky_editor_area_checks_t *checks) {
    return checks->valid[0] && checks->valid[1] &&
           checks->valid[2] && checks->valid[3];
}

static bool counter_is_newer(uint8_t candidate, uint8_t current) {
    uint8_t distance = (uint8_t)(candidate - current);
    return distance != 0 && distance < 128;
}

/* Decode the 42 encrypted data blocks into a caller-owned working copy. */
static bool decode_editor_save_sectors(uint8_t dump[SKY_DUMP_SIZE]) {
    uint8_t material[32 + 1 + sizeof(k_editor_key_magic) - 1];
    uint8_t key[16];
    uint8_t decoded[16];
    mbedtls_aes_context aes;

    memcpy(material, dump, 32);
    memcpy(material + 33, k_editor_key_magic, sizeof(k_editor_key_magic) - 1);
    mbedtls_aes_init(&aes);
    for (uint8_t block = 0x08; block <= 0x3e; block++) {
        if ((block & 3) == 3) continue; /* MIFARE sector trailer */
        material[32] = block;
        if (mbedtls_md5(material, sizeof(material), key) != 0 ||
            mbedtls_aes_setkey_dec(&aes, key, 128) != 0 ||
            mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT,
                                  &dump[block * 16], decoded) != 0) {
            mbedtls_aes_free(&aes);
            return false;
        }
        memcpy(&dump[block * 16], decoded, sizeof(decoded));
    }
    mbedtls_aes_free(&aes);
    return true;
}

/* The inverse of decode_editor_save_sectors().  It uses the same per-block
 * MD5 key material, but AES-ECB encryption, and deliberately skips MIFARE
 * trailer blocks exactly as the verified read-only inspector does. */
static bool encode_editor_save_sectors(uint8_t dump[SKY_DUMP_SIZE]) {
    uint8_t material[32 + 1 + sizeof(k_editor_key_magic) - 1];
    uint8_t key[16];
    uint8_t encoded[16];
    mbedtls_aes_context aes;

    memcpy(material, dump, 32);
    memcpy(material + 33, k_editor_key_magic, sizeof(k_editor_key_magic) - 1);
    mbedtls_aes_init(&aes);
    for (uint8_t block = 0x08; block <= 0x3e; block++) {
        if ((block & 3) == 3) continue;
        material[32] = block;
        if (mbedtls_md5(material, sizeof(material), key) != 0 ||
            mbedtls_aes_setkey_enc(&aes, key, 128) != 0 ||
            mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT,
                                  &dump[block * 16], encoded) != 0) {
            mbedtls_aes_free(&aes);
            return false;
        }
        memcpy(&dump[block * 16], encoded, sizeof(encoded));
    }
    mbedtls_aes_free(&aes);
    return true;
}

static void update_area_checks(uint8_t dump[SKY_DUMP_SIZE], size_t base) {
    uint16_t crc;

    crc = crc16_bytes(0xffff, &dump[base], 0x0e);
    crc = crc16_update(crc, 0x05);
    crc = crc16_update(crc, 0x00);
    dump[base + 0x0e] = (uint8_t)crc;
    dump[base + 0x0f] = (uint8_t)(crc >> 8);

    crc = crc16_bytes(0xffff, &dump[base + 0x10], 0x20);
    crc = crc16_bytes(crc, &dump[base + 0x40], 0x10);
    dump[base + 0x0c] = (uint8_t)crc;
    dump[base + 0x0d] = (uint8_t)(crc >> 8);

    crc = crc16_bytes(0xffff, &dump[base + 0x50], 0x20);
    crc = crc16_bytes(crc, &dump[base + 0x80], 0x10);
    for (int i = 0; i < 224; i++) crc = crc16_update(crc, 0);
    dump[base + 0x0a] = (uint8_t)crc;
    dump[base + 0x0b] = (uint8_t)(crc >> 8);

    crc = 0xffff;
    crc = crc16_update(crc, 0x06);
    crc = crc16_update(crc, 0x01);
    crc = crc16_bytes(crc, &dump[base + 0x92], 0x1e);
    crc = crc16_bytes(crc, &dump[base + 0xc0], 0x20);
    dump[base + 0x90] = (uint8_t)crc;
    dump[base + 0x91] = (uint8_t)(crc >> 8);
}

static bool read_exact_dump(const char *path, uint8_t dump[SKY_DUMP_SIZE]) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t count = fread(dump, 1, SKY_DUMP_SIZE, file);
    int extra = fgetc(file);
    fclose(file);
    return count == SKY_DUMP_SIZE && extra == EOF;
}

static bool write_exact_dump(const char *path, const uint8_t dump[SKY_DUMP_SIZE]) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    size_t count = fwrite(dump, 1, SKY_DUMP_SIZE, file);
    int flush_result = fflush(file);
    int close_result = fclose(file);
    return count == SKY_DUMP_SIZE && flush_result == 0 && close_result == 0;
}

static bool copy_exact_dump(const char *source, const char *destination) {
    uint8_t *dump = malloc(SKY_DUMP_SIZE);
    if (!dump) return false;
    bool ok = read_exact_dump(source, dump) && write_exact_dump(destination, dump);
    free(dump);
    return ok;
}

static bool make_editor_path(const library_entry_t *entry, const char *suffix,
                             char *out, size_t out_size) {
    int written = snprintf(out, out_size, "%s%s", entry->path, suffix);
    return written > 0 && (size_t)written < out_size;
}

static bool dump_has_blank_save_areas(const uint8_t dump[SKY_DUMP_SIZE]) {
    for (size_t base = AREA_A_BASE; base <= AREA_B_BASE; base += AREA_B_BASE - AREA_A_BASE) {
        /* Gold, counter, and the four normal-save checksum fields are zero
         * in the known unused factory templates. */
        if (read_le16(&dump[base + 0x03]) != 0 || dump[base + 0x09] != 0 ||
            read_le16(&dump[base + 0x0a]) != 0 ||
            read_le16(&dump[base + 0x0c]) != 0 ||
            read_le16(&dump[base + 0x0e]) != 0 ||
            read_le16(&dump[base + 0x90]) != 0) return false;
    }
    return true;
}

static bool field_equals(const char *value, const char *expected) {
    return value && expected && strcasecmp(value, expected) == 0;
}

sky_editor_figure_class_t sky_editor_entry_class(const library_entry_t *entry) {
    if (!entry || !entry_path_is_approved(entry)) return SKY_EDITOR_UNSUPPORTED;
    if (field_equals(entry->category, "Trap") || field_equals(entry->type, "Trap"))
        return SKY_EDITOR_TRAP;
    if (field_equals(entry->category, "Vehicle") || field_equals(entry->type, "Vehicle"))
        return SKY_EDITOR_VEHICLE;
    if (field_equals(entry->category, "Creation Crystal") ||
        field_equals(entry->type, "Creation Crystal"))
        return SKY_EDITOR_CREATION_CRYSTAL;
    if (field_equals(entry->category, "Item") ||
        field_equals(entry->category, "Adventure Pack") ||
        field_equals(entry->type, "Item") || field_equals(entry->type, "Adventure Pack"))
        return SKY_EDITOR_ITEM;
    if (field_equals(entry->category, "Sidekick") || field_equals(entry->type, "Sidekick"))
        return SKY_EDITOR_SIDEKICK;
    /* The present Imaginators corpus consists of Sensei-style figures. They
     * use a familiar-looking layout, but remain disabled until an in-game
     * saved Sensei independently confirms the interpretation. */
    if (field_equals(entry->game, "Imaginators") && field_equals(entry->category, "Skylander"))
        return SKY_EDITOR_SENSEI;
    if (field_equals(entry->category, "Skylander")) return SKY_EDITOR_NORMAL_CHARACTER;
    /* User uploads deliberately have incomplete metadata. Their decoded
     * normal-save checksum structure must prove the final classification. */
    if (field_equals(entry->source, "user")) return SKY_EDITOR_UNKNOWN;
    return SKY_EDITOR_UNSUPPORTED;
}

const char *sky_editor_figure_class_name(sky_editor_figure_class_t figure_class) {
    switch (figure_class) {
        case SKY_EDITOR_NORMAL_CHARACTER: return "normal character";
        case SKY_EDITOR_TRAP: return "trap";
        case SKY_EDITOR_VEHICLE: return "vehicle";
        case SKY_EDITOR_CREATION_CRYSTAL: return "creation crystal";
        case SKY_EDITOR_ITEM: return "item";
        case SKY_EDITOR_SIDEKICK: return "sidekick";
        case SKY_EDITOR_SENSEI: return "sensei";
        default: return "unknown";
    }
}

static sky_editor_generation_t generation_from_name(const char *game) {
    if (field_equals(game, "Spyros Adventure") || field_equals(game, "Spyro's Adventure"))
        return SKY_GENERATION_SSA;
    if (field_equals(game, "Giants")) return SKY_GENERATION_GIANTS;
    if (field_equals(game, "Swapforce") || field_equals(game, "Swap Force"))
        return SKY_GENERATION_SWAP_FORCE;
    if (field_equals(game, "Trap Team")) return SKY_GENERATION_TRAP_TEAM;
    if (field_equals(game, "Superchargers") || field_equals(game, "SuperChargers"))
        return SKY_GENERATION_SUPERCHARGERS;
    if (field_equals(game, "Imaginators")) return SKY_GENERATION_IMAGINATORS;
    return SKY_GENERATION_UNKNOWN;
}

static sky_editor_generation_t generation_from_character_id(uint16_t id) {
    if (id <= 31) return SKY_GENERATION_SSA;
    if (id <= 39) return SKY_GENERATION_GIANTS;
    if (id <= 67) return SKY_GENERATION_SWAP_FORCE;
    if (id <= 97) return SKY_GENERATION_TRAP_TEAM;
    if (id <= 106) return SKY_GENERATION_SUPERCHARGERS;
    if (id >= 108 && id <= 255) return SKY_GENERATION_IMAGINATORS;
    return SKY_GENERATION_UNKNOWN;
}

static bool is_unverified_sensei_id(uint16_t id) {
    /* IDs observed in the packaged Imaginators Sensei corpus. Keep these
     * disabled for User Added files as well until an in-game saved Sensei is
     * independently validated. Creation Crystals are 680..689 and fail the
     * normal-save structural check separately. */
    return id >= 601 && id <= 631;
}

const char *sky_editor_generation_name(sky_editor_generation_t generation) {
    switch (generation) {
        case SKY_GENERATION_SSA: return "Spyros Adventure";
        case SKY_GENERATION_GIANTS: return "Giants";
        case SKY_GENERATION_SWAP_FORCE: return "Swap Force";
        case SKY_GENERATION_TRAP_TEAM: return "Trap Team";
        case SKY_GENERATION_SUPERCHARGERS: return "SuperChargers";
        case SKY_GENERATION_IMAGINATORS: return "Imaginators";
        default: return "Unknown";
    }
}

bool sky_editor_entry_supported(const library_entry_t *entry) {
    sky_editor_figure_class_t figure_class = sky_editor_entry_class(entry);
    return figure_class == SKY_EDITOR_NORMAL_CHARACTER ||
           figure_class == SKY_EDITOR_UNKNOWN;
}

esp_err_t sky_editor_inspect_file(const library_entry_t *entry,
                                  sky_editor_info_t *result) {
    if (!entry || !result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));

    result->figure_class = sky_editor_entry_class(entry);
    if (!entry_path_is_approved(entry)) {
        set_error(result, "The selected library path is not approved.");
        return ESP_ERR_INVALID_ARG;
    }
    if (result->figure_class != SKY_EDITOR_NORMAL_CHARACTER &&
        result->figure_class != SKY_EDITOR_UNKNOWN) {
        set_error(result, "Normal-character inspection is not available for %s files yet.",
                  sky_editor_figure_class_name(result->figure_class));
        return ESP_ERR_NOT_SUPPORTED;
    }

    struct stat status;
    if (stat(entry->path, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size != SKY_DUMP_SIZE) {
        set_error(result, "The Skylander dump must be exactly 1,024 bytes.");
        return ESP_ERR_INVALID_SIZE;
    }

    /* This function runs in ESP-IDF's HTTP-server task.  Keep the two dump
     * copies off that task's stack: AES inspection needs both the exact raw
     * bytes and a decoded working copy, which is 2 KiB before crypto-call
     * stack use. */
    uint8_t *raw = malloc(SKY_DUMP_SIZE);
    uint8_t *decoded = malloc(SKY_DUMP_SIZE);
    if (!raw || !decoded) {
        free(decoded);
        free(raw);
        set_error(result, "Not enough memory to inspect this dump.");
        return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(entry->path, "rb");
    if (!file) {
        free(decoded);
        free(raw);
        set_error(result, "The Skylander dump could not be opened.");
        return ESP_FAIL;
    }
    size_t count = fread(raw, 1, SKY_DUMP_SIZE, file);
    int extra = fgetc(file);
    fclose(file);
    if (count != SKY_DUMP_SIZE || extra != EOF) {
        free(decoded);
        free(raw);
        set_error(result, "The Skylander dump could not be read safely.");
        return ESP_FAIL;
    }

    /* The identity header is plaintext in the verified normal layout. */
    result->character_id = read_le16(&raw[0x10]);
    if (raw[0] == 0 || raw[0] == 0xff) {
        free(decoded);
        free(raw);
        result->figure_class = SKY_EDITOR_UNKNOWN;
        set_error(result, "The dump does not contain a plausible normal-character header.");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (is_unverified_sensei_id(result->character_id)) {
        free(decoded);
        free(raw);
        result->figure_class = SKY_EDITOR_SENSEI;
        set_error(result, "Sensei inspection remains disabled until a real saved Sensei is independently verified.");
        return ESP_ERR_NOT_SUPPORTED;
    }
    result->generation = generation_from_name(entry->game);
    if (result->generation == SKY_GENERATION_UNKNOWN)
        result->generation = generation_from_character_id(result->character_id);
    /*
     * The portal never validates or recalculates this field: it replays the
     * 1,024 raw bytes from SPIFFS.  In particular, the known-good factory
     * Lightning Rod dump has a non-matching value here under the legacy
     * SkyEditGUI CRC model.  Keep this as evidence for the debug response,
     * never as an admission test for a dump the game itself accepts.
     */
    result->header_valid = inspect_header_crc(raw, result);

    memcpy(decoded, raw, SKY_DUMP_SIZE);
    bool decode_ok = decode_editor_save_sectors(decoded);
    sky_editor_area_checks_t decoded_a, decoded_b, raw_a, raw_b;
    inspect_area_checks(raw, AREA_A_BASE, &raw_a);
    inspect_area_checks(raw, AREA_B_BASE, &raw_b);
    if (decode_ok) {
        inspect_area_checks(decoded, AREA_A_BASE, &decoded_a);
        inspect_area_checks(decoded, AREA_B_BASE, &decoded_b);
    } else {
        memset(&decoded_a, 0, sizeof(decoded_a));
        memset(&decoded_b, 0, sizeof(decoded_b));
    }

    const uint8_t *view = NULL;
    if (decode_ok && (all_checks_valid(&decoded_a) || all_checks_valid(&decoded_b))) {
        /* Confirmed by the supplied saved Lightning Rod: this branch decodes
         * both A and B to Gold=2, with valid normal-save checksums. */
        view = decoded;
        result->encrypted = true;
        result->area_a_checks = decoded_a;
        result->area_b_checks = decoded_b;
    } else if (all_checks_valid(&raw_a) || all_checks_valid(&raw_b) ||
               dump_has_blank_save_areas(raw)) {
        /* Unused templates are deliberately blank, and are kept as-is. */
        view = raw;
        result->encrypted = false;
        result->area_a_checks = raw_a;
        result->area_b_checks = raw_b;
    } else {
        free(decoded);
        free(raw);
        result->figure_class = SKY_EDITOR_UNKNOWN;
        set_error(result, "Unsupported or malformed save-area representation.");
        return ESP_ERR_INVALID_CRC;
    }

    result->area_a_counter = view[AREA_A_BASE + 0x09];
    result->area_b_counter = view[AREA_B_BASE + 0x09];
    result->area_a_gold = read_le16(&view[AREA_A_BASE + 0x03]);
    result->area_b_gold = read_le16(&view[AREA_B_BASE + 0x03]);
    result->area_a_valid = all_checks_valid(&result->area_a_checks);
    result->area_b_valid = all_checks_valid(&result->area_b_checks);
    result->area_a_progression = inspect_progression(view, AREA_A_BASE);
    result->area_b_progression = inspect_progression(view, AREA_B_BASE);

    /* The portal/game path stores and replays raw bytes and does not use the
     * SkyEditGUI checksum layout. Keep the individual checks diagnostic-only
     * until they are confirmed against a real saved corpus. */
    if (result->area_a_valid && !result->area_b_valid) {
        result->selected_area = 'A';
        result->selected_gold = result->area_a_gold;
    } else if (!result->area_a_valid && result->area_b_valid) {
        result->selected_area = 'B';
        result->selected_gold = result->area_b_gold;
    } else if (counter_is_newer(result->area_b_counter, result->area_a_counter)) {
        result->selected_area = 'B';
        result->selected_gold = result->area_b_gold;
    } else {
        result->selected_area = 'A';
        result->selected_gold = result->area_a_gold;
    }
    const sky_editor_progression_t *active_progression =
        result->selected_area == 'A' ? &result->area_a_progression :
                                       &result->area_b_progression;
    result->derived_level = derived_level_from_progression(active_progression->total);
    result->derived_level_provisional = false;
    bool blank_template = !result->encrypted && result->area_a_checks.uninitialised &&
                          result->area_b_checks.uninitialised &&
        result->area_a_counter == 0 && result->area_b_counter == 0 &&
        result->area_a_gold == 0 && result->area_b_gold == 0;
    if (blank_template && field_equals(entry->source, "user")) {
        free(decoded);
        free(raw);
        result->figure_class = SKY_EDITOR_UNKNOWN;
        set_error(result, "An uninitialised User Added dump cannot be proven to use the normal-character layout.");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (blank_template && result->generation == SKY_GENERATION_SWAP_FORCE &&
        (strstr(entry->name, "(Top)") || strstr(entry->name, "(Bottom)"))) {
        free(decoded);
        free(raw);
        result->figure_class = SKY_EDITOR_UNKNOWN;
        set_error(result, "An uninitialised Swap Force half cannot be proven to contain normal progression data.");
        return ESP_ERR_NOT_SUPPORTED;
    }
    result->figure_class = SKY_EDITOR_NORMAL_CHARACTER;
    result->supported = true;
    if (blank_template) {
        snprintf(result->state, sizeof(result->state), "uninitialised");
        snprintf(result->warning, sizeof(result->warning),
                 "Save data has not been initialised yet.");
    } else if (result->area_a_valid || result->area_b_valid) {
        snprintf(result->state, sizeof(result->state), "validated");
    } else {
        snprintf(result->state, sizeof(result->state), "legacy-raw");
        snprintf(result->warning, sizeof(result->warning),
                 "Gold read from save area %c. Legacy checksum tests did not match this raw portal dump.",
                 result->selected_area);
    }
    free(decoded);
    free(raw);
    return ESP_OK;
}

bool sky_editor_gold_editable(const sky_editor_info_t *info) {
    if (!info || !info->supported) return false;
    return (info->selected_area == 'A' && info->area_a_valid) ||
           (info->selected_area == 'B' && info->area_b_valid);
}

bool sky_editor_level_editable(const sky_editor_info_t *info) {
    return sky_editor_gold_editable(info);
}

bool sky_editor_backup_exists(const library_entry_t *entry) {
    if (!entry || !entry_path_is_approved(entry)) return false;
    char backup_path[sizeof(entry->path) + 5];
    struct stat status;
    return make_editor_path(entry, ".bak", backup_path, sizeof(backup_path)) &&
           stat(backup_path, &status) == 0 && S_ISREG(status.st_mode) &&
           status.st_size == SKY_DUMP_SIZE;
}

static bool inspection_matches_edit(const sky_editor_info_t *before,
                                    const sky_editor_info_t *after,
                                    uint16_t gold) {
    if (!sky_editor_gold_editable(after) || after->selected_gold != gold ||
        after->selected_area != before->selected_area ||
        after->encrypted != before->encrypted) return false;
    return before->selected_area == 'A' ? after->area_a_valid : after->area_b_valid;
}

static bool inspection_matches_level_edit(const sky_editor_info_t *before,
                                          const sky_editor_info_t *after,
                                          uint8_t level) {
    sky_editor_progression_t expected;
    if (!sky_editor_level_editable(after) || !level_progression(level, &expected) ||
        after->selected_area != before->selected_area ||
        after->encrypted != before->encrypted || after->derived_level != level)
        return false;
    const sky_editor_progression_t *actual = after->selected_area == 'A'
        ? &after->area_a_progression : &after->area_b_progression;
    if (actual->component_1 != expected.component_1 ||
        actual->component_2 != expected.component_2 ||
        actual->component_3 != expected.component_3 ||
        actual->total != expected.total)
        return false;
    return before->selected_area == 'A' ? after->area_a_valid : after->area_b_valid;
}

esp_err_t sky_editor_save_gold(const library_entry_t *entry, uint16_t gold,
                               sky_editor_info_t *result) {
    if (!entry || !result) return ESP_ERR_INVALID_ARG;
    sky_editor_info_t before;
    esp_err_t inspect_result = sky_editor_inspect_file(entry, &before);
    if (inspect_result != ESP_OK) {
        *result = before;
        return inspect_result;
    }
    if (!sky_editor_gold_editable(&before)) {
        *result = before;
        set_error(result, "Gold can be edited only in a checksum-validated active save area.");
        return ESP_ERR_NOT_SUPPORTED;
    }

    char temp_path[sizeof(entry->path) + 5];
    char backup_path[sizeof(entry->path) + 5];
    if (!make_editor_path(entry, ".tmp", temp_path, sizeof(temp_path)) ||
        !make_editor_path(entry, ".bak", backup_path, sizeof(backup_path))) {
        *result = before;
        set_error(result, "The dump path is too long to create a safe edit file.");
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *working = malloc(SKY_DUMP_SIZE);
    if (!working) {
        *result = before;
        set_error(result, "Not enough memory to edit this dump.");
        return ESP_ERR_NO_MEM;
    }
    if (!read_exact_dump(entry->path, working)) {
        free(working);
        *result = before;
        set_error(result, "The original dump could not be read safely.");
        return ESP_FAIL;
    }
    if (before.encrypted && !decode_editor_save_sectors(working)) {
        free(working);
        *result = before;
        set_error(result, "The saved dump could not be decrypted safely.");
        return ESP_FAIL;
    }

    size_t active_base = before.selected_area == 'A' ? AREA_A_BASE : AREA_B_BASE;
    working[active_base + 0x03] = (uint8_t)gold;
    working[active_base + 0x04] = (uint8_t)(gold >> 8);
    update_area_checks(working, active_base);
    if (before.encrypted && !encode_editor_save_sectors(working)) {
        free(working);
        *result = before;
        set_error(result, "The updated dump could not be encrypted safely.");
        return ESP_FAIL;
    }

    /* A stale temporary file is never used as input; it is replaced and then
     * fully re-inspected before the original is moved. */
    remove(temp_path);
    bool write_ok = write_exact_dump(temp_path, working);
    free(working);
    if (!write_ok) {
        remove(temp_path);
        *result = before;
        set_error(result, "The temporary dump could not be written safely.");
        return ESP_FAIL;
    }

    library_entry_t temporary_entry = *entry;
    if (snprintf(temporary_entry.path, sizeof(temporary_entry.path), "%s", temp_path) >=
        (int)sizeof(temporary_entry.path)) {
        remove(temp_path);
        *result = before;
        set_error(result, "The temporary dump path is too long to verify.");
        return ESP_ERR_INVALID_SIZE;
    }
    sky_editor_info_t temporary_info;
    inspect_result = sky_editor_inspect_file(&temporary_entry, &temporary_info);
    if (inspect_result != ESP_OK ||
        !inspection_matches_edit(&before, &temporary_info, gold)) {
        remove(temp_path);
        *result = temporary_info;
        set_error(result, "Temporary-file verification failed; the original dump was not changed.");
        return ESP_ERR_INVALID_CRC;
    }

    /* Keep one backup only. The original is never removed until the complete
     * .tmp file has passed the same inspector used by the read-only view. */
    remove(backup_path);
    if (rename(entry->path, backup_path) != 0) {
        remove(temp_path);
        *result = before;
        set_error(result, "The original dump could not be backed up.");
        return ESP_FAIL;
    }
    if (rename(temp_path, entry->path) != 0) {
        bool restored = copy_exact_dump(backup_path, entry->path);
        remove(temp_path);
        *result = before;
        set_error(result, restored ?
                  "The updated dump could not be installed; the backup was restored." :
                  "The updated dump could not be installed; the backup is still available.");
        return ESP_FAIL;
    }

    /* This is intentionally a second read of the installed .sky file, not
     * reuse of temporary_info. It fulfills the final on-flash verification
     * before reporting success to the browser. */
    inspect_result = sky_editor_inspect_file(entry, result);
    if (inspect_result != ESP_OK || !inspection_matches_edit(&before, result, gold)) {
        bool restored = copy_exact_dump(backup_path, entry->path);
        if (restored) sky_editor_inspect_file(entry, result);
        set_error(result, restored ?
                  "Installed-file verification failed; the backup was restored." :
                  "Installed-file verification failed; the backup remains available.");
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

esp_err_t sky_editor_save_level(const library_entry_t *entry, uint8_t level,
                                sky_editor_info_t *result) {
    if (!entry || !result) return ESP_ERR_INVALID_ARG;
    sky_editor_progression_t requested;
    if (!level_progression(level, &requested)) {
        memset(result, 0, sizeof(*result));
        set_error(result, "Level must be a whole number from 1 to 20.");
        return ESP_ERR_INVALID_ARG;
    }

    sky_editor_info_t before;
    esp_err_t inspect_result = sky_editor_inspect_file(entry, &before);
    if (inspect_result != ESP_OK) {
        *result = before;
        return inspect_result;
    }
    if (!sky_editor_level_editable(&before)) {
        *result = before;
        set_error(result, "Level can be edited only in a checksum-validated active save area.");
        return ESP_ERR_NOT_SUPPORTED;
    }

    char temp_path[sizeof(entry->path) + 5];
    char backup_path[sizeof(entry->path) + 5];
    if (!make_editor_path(entry, ".tmp", temp_path, sizeof(temp_path)) ||
        !make_editor_path(entry, ".bak", backup_path, sizeof(backup_path))) {
        *result = before;
        set_error(result, "The dump path is too long to create a safe edit file.");
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *working = malloc(SKY_DUMP_SIZE);
    if (!working) {
        *result = before;
        set_error(result, "Not enough memory to edit this dump.");
        return ESP_ERR_NO_MEM;
    }
    if (!read_exact_dump(entry->path, working)) {
        free(working);
        *result = before;
        set_error(result, "The original dump could not be read safely.");
        return ESP_FAIL;
    }
    if (before.encrypted && !decode_editor_save_sectors(working)) {
        free(working);
        *result = before;
        set_error(result, "The saved dump could not be decrypted safely.");
        return ESP_FAIL;
    }

    size_t active_base = before.selected_area == 'A' ? AREA_A_BASE : AREA_B_BASE;
    write_progression(working, active_base, &requested);
    update_area_checks(working, active_base);
    if (before.encrypted && !encode_editor_save_sectors(working)) {
        free(working);
        *result = before;
        set_error(result, "The updated dump could not be encrypted safely.");
        return ESP_FAIL;
    }

    remove(temp_path);
    bool write_ok = write_exact_dump(temp_path, working);
    free(working);
    if (!write_ok) {
        remove(temp_path);
        *result = before;
        set_error(result, "The temporary dump could not be written safely.");
        return ESP_FAIL;
    }

    library_entry_t temporary_entry = *entry;
    if (snprintf(temporary_entry.path, sizeof(temporary_entry.path), "%s", temp_path) >=
        (int)sizeof(temporary_entry.path)) {
        remove(temp_path);
        *result = before;
        set_error(result, "The temporary dump path is too long to verify.");
        return ESP_ERR_INVALID_SIZE;
    }
    sky_editor_info_t temporary_info;
    inspect_result = sky_editor_inspect_file(&temporary_entry, &temporary_info);
    if (inspect_result != ESP_OK ||
        !inspection_matches_level_edit(&before, &temporary_info, level)) {
        remove(temp_path);
        *result = temporary_info;
        set_error(result, "Temporary-file verification failed; the original dump was not changed.");
        return ESP_ERR_INVALID_CRC;
    }

    remove(backup_path);
    if (rename(entry->path, backup_path) != 0) {
        remove(temp_path);
        *result = before;
        set_error(result, "The original dump could not be backed up.");
        return ESP_FAIL;
    }
    if (rename(temp_path, entry->path) != 0) {
        bool restored = copy_exact_dump(backup_path, entry->path);
        remove(temp_path);
        *result = before;
        set_error(result, restored ?
                  "The updated dump could not be installed; the backup was restored." :
                  "The updated dump could not be installed; the backup is still available.");
        return ESP_FAIL;
    }

    inspect_result = sky_editor_inspect_file(entry, result);
    if (inspect_result != ESP_OK ||
        !inspection_matches_level_edit(&before, result, level)) {
        bool restored = copy_exact_dump(backup_path, entry->path);
        if (restored) sky_editor_inspect_file(entry, result);
        set_error(result, restored ?
                  "Installed-file verification failed; the backup was restored." :
                  "Installed-file verification failed; the backup remains available.");
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

esp_err_t sky_editor_restore_backup(const library_entry_t *entry,
                                    sky_editor_info_t *result) {
    if (!entry || !result || !entry_path_is_approved(entry)) return ESP_ERR_INVALID_ARG;
    char temp_path[sizeof(entry->path) + 5];
    char backup_path[sizeof(entry->path) + 5];
    char rollback_path[sizeof(entry->path) + 9];
    if (!make_editor_path(entry, ".tmp", temp_path, sizeof(temp_path)) ||
        !make_editor_path(entry, ".bak", backup_path, sizeof(backup_path)) ||
        !make_editor_path(entry, ".restore", rollback_path, sizeof(rollback_path)) ||
        !sky_editor_backup_exists(entry)) {
        set_error(result, "No valid backup is available for this dump.");
        return ESP_ERR_NOT_FOUND;
    }

    remove(temp_path);
    remove(rollback_path);
    if (!copy_exact_dump(backup_path, temp_path)) {
        set_error(result, "The backup could not be copied safely.");
        return ESP_FAIL;
    }
    library_entry_t temporary_entry = *entry;
    if (snprintf(temporary_entry.path, sizeof(temporary_entry.path), "%s", temp_path) >=
        (int)sizeof(temporary_entry.path) ||
        sky_editor_inspect_file(&temporary_entry, result) != ESP_OK ||
        !sky_editor_gold_editable(result)) {
        remove(temp_path);
        set_error(result, "The backup did not pass inspector verification.");
        return ESP_ERR_INVALID_CRC;
    }
    const uint16_t expected_gold = result->selected_gold;

    if (rename(entry->path, rollback_path) != 0) {
        remove(temp_path);
        set_error(result, "The current dump could not be staged for restore.");
        return ESP_FAIL;
    }
    if (rename(temp_path, entry->path) != 0) {
        rename(rollback_path, entry->path);
        remove(temp_path);
        set_error(result, "The backup could not be installed; the current dump was kept.");
        return ESP_FAIL;
    }
    esp_err_t verify_result = sky_editor_inspect_file(entry, result);
    if (verify_result != ESP_OK || result->selected_gold != expected_gold ||
        !sky_editor_gold_editable(result)) {
        remove(entry->path);
        rename(rollback_path, entry->path);
        set_error(result, "Restored-file verification failed; the current dump was kept.");
        return ESP_ERR_INVALID_CRC;
    }
    remove(rollback_path);
    return ESP_OK;
}
