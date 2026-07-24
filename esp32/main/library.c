#include "library.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"

static const char *TAG = "Library";
static int s_entry_count = -1;
static int s_user_count = -1;
#define LIBRARY_MAX_FAVOURITES 768
static uint64_t s_favourite_hashes[LIBRARY_MAX_FAVOURITES];
static int s_favourite_count;
static bool s_favourites_loaded;
static void library_prune_favourites(void);
static bool favourite_line_key(const char *line, char *out, size_t out_size);

/* Keep the on-flash favourites list persistent, but keep its compact identity
 * set in RAM.  This avoids reopening SPIFFS once for every card rendered. */
static uint64_t favourite_hash(const char *text) {
    uint64_t hash = UINT64_C(1469598103934665603);
    while (*text) { hash ^= (uint8_t)*text++; hash *= UINT64_C(1099511628211); }
    return hash;
}

static void favourite_cache_load(void) {
    if (s_favourites_loaded) return;
    s_favourites_loaded = true;
    s_favourite_count = 0;
    FILE *f = fopen(LIBRARY_FAVOURITES_PATH, "rb");
    if (!f) return;
    char line[340], key[300];
    while (s_favourite_count < LIBRARY_MAX_FAVOURITES && fgets(line, sizeof(line), f)) {
        if (favourite_line_key(line, key, sizeof(key)))
            s_favourite_hashes[s_favourite_count++] = favourite_hash(key);
    }
    fclose(f);
}

static bool has_sky_extension(const char *name) {
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".sky") == 0;
}

static bool is_element(const char *name) {
    static const char *const elements[] = {
        "Magic", "Water", "Fire", "Life", "Undead", "Earth", "Air",
        "Tech", "Light", "Dark", "Kaos"
    };
    for (size_t i = 0; i < sizeof(elements) / sizeof(elements[0]); i++)
        if (strcasecmp(name, elements[i]) == 0) return true;
    return false;
}

static void copy_field(char *out, size_t out_size, const char *value) {
    if (!value) value = "";
    snprintf(out, out_size, "%s", value);
}

static void basename_without_extension(const char *path, char *out, size_t out_size) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    copy_field(out, out_size, base);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

/* JSON strings in the supplied library names do not require more than the
 * usual quote/backslash escaping. */
static void json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

static bool is_alternate_types(const char *name) {
    return strcasecmp(name, "Alternative Types") == 0 ||
           strcasecmp(name, "Alternate Types") == 0;
}

static void set_variant_from_parts(library_entry_t *entry, char *parts[], size_t folders) {
    for (size_t i = 1; i < folders; i++) {
        if (is_alternate_types(parts[i])) {
            copy_field(entry->variant, sizeof(entry->variant), entry->name);
            return;
        }
    }
}

static void element_from_crystal_name(const char *name, char *out, size_t out_size) {
    char word[16];
    size_t used = 0;
    for (const char *p = name;; p++) {
        if (isalpha((unsigned char)*p)) {
            if (used + 1 < sizeof(word)) word[used++] = *p;
        } else {
            if (used) {
                word[used] = '\0';
                if (is_element(word)) {
                    copy_field(out, out_size, word);
                    return;
                }
                used = 0;
            }
            if (!*p) return;
        }
    }
}

static void classify_path(const char *full_path, library_entry_t *entry) {
    memset(entry, 0, sizeof(*entry));
    copy_field(entry->path, sizeof(entry->path), full_path);
    copy_field(entry->type, sizeof(entry->type), "Normal");
    copy_field(entry->variant, sizeof(entry->variant), "Normal");
    copy_field(entry->category, sizeof(entry->category), "Skylander");
    copy_field(entry->source, sizeof(entry->source), "factory");
    basename_without_extension(full_path, entry->name, sizeof(entry->name));

    const char *relative = full_path + strlen(LIBRARY_ROOT);
    while (*relative == '/') relative++;
    char path_parts[256];
    copy_field(path_parts, sizeof(path_parts), relative);
    char *parts[16];
    size_t part_count = 0;
    char *save = NULL;
    for (char *part = strtok_r(path_parts, "/", &save);
         part && part_count < sizeof(parts) / sizeof(parts[0]);
         part = strtok_r(NULL, "/", &save)) {
        parts[part_count++] = part;
    }
    if (!part_count) return;
    const size_t folders = part_count - 1; /* final component is the .sky file */
    const char *top = parts[0];

    if (strcasecmp(top, "Items") == 0 || strcasecmp(top, "Adventure Packs") == 0) {
        bool items = strcasecmp(top, "Items") == 0;
        copy_field(entry->category, sizeof(entry->category), items ? "Item" : "Adventure Pack");
        copy_field(entry->type, sizeof(entry->type), items ? "Item" : "Adventure Pack");
        if (folders > 1) copy_field(entry->game, sizeof(entry->game), parts[1]);
        set_variant_from_parts(entry, parts, folders);
        return;
    }

    if (strcasecmp(top, "Sidekicks") == 0) {
        copy_field(entry->category, sizeof(entry->category), "Sidekick");
        copy_field(entry->type, sizeof(entry->type), "Sidekick");
        for (size_t i = 1; i < folders; i++) {
            if (is_element(parts[i])) copy_field(entry->element, sizeof(entry->element), parts[i]);
            else if (!entry->game[0] && !is_alternate_types(parts[i]))
                copy_field(entry->game, sizeof(entry->game), parts[i]);
        }
        set_variant_from_parts(entry, parts, folders);
        return;
    }

    copy_field(entry->game, sizeof(entry->game), top);
    if (folders > 1) {
        const char *second = parts[1];
        if (is_element(second)) copy_field(entry->element, sizeof(entry->element), second);
        else if (strcasecmp(second, "Giants") == 0) copy_field(entry->type, sizeof(entry->type), "Giant");
        else copy_field(entry->type, sizeof(entry->type), second);
    }

    for (size_t i = 1; i < folders; i++) {
        if (is_element(parts[i])) copy_field(entry->element, sizeof(entry->element), parts[i]);
        if (strcasecmp(parts[i], "Traps") == 0) {
            copy_field(entry->category, sizeof(entry->category), "Trap");
            copy_field(entry->type, sizeof(entry->type), "Trap");
        } else if (strcasecmp(parts[i], "Vehicle") == 0 ||
                   strcasecmp(parts[i], "Vehicles") == 0) {
            copy_field(entry->category, sizeof(entry->category), "Vehicle");
            copy_field(entry->type, sizeof(entry->type), "Vehicle");
        } else if (strcasecmp(parts[i], "Creation Crystals") == 0) {
            copy_field(entry->category, sizeof(entry->category), "Creation Crystal");
            copy_field(entry->type, sizeof(entry->type), "Creation Crystal");
            if (!entry->element[0])
                element_from_crystal_name(entry->name, entry->element, sizeof(entry->element));
        } else if (strcasecmp(parts[i], "Sidekicks") == 0) {
            copy_field(entry->category, sizeof(entry->category), "Sidekick");
            copy_field(entry->type, sizeof(entry->type), "Sidekick");
        }
    }
    set_variant_from_parts(entry, parts, folders);
}

static void classify_user_path(const char *full_path, library_entry_t *entry) {
    memset(entry, 0, sizeof(*entry));
    copy_field(entry->path, sizeof(entry->path), full_path);
    copy_field(entry->variant, sizeof(entry->variant), "User Upload");
    copy_field(entry->category, sizeof(entry->category), "User Added");
    copy_field(entry->source, sizeof(entry->source), "user");
    basename_without_extension(full_path, entry->name, sizeof(entry->name));
}

static bool write_entry(FILE *index, const library_entry_t *entry, bool first) {
    if (!first) fputs(",\n", index);
    fputs("{\"id\":", index); fprintf(index, "%u", (unsigned)entry->id);
    fputs(",\"name\":", index);     json_string(index, entry->name);
    fputs(",\"game\":", index);     json_string(index, entry->game);
    fputs(",\"element\":", index);  json_string(index, entry->element);
    fputs(",\"type\":", index);     json_string(index, entry->type);
    fputs(",\"variant\":", index);  json_string(index, entry->variant);
    fputs(",\"category\":", index); json_string(index, entry->category);
    fputs(",\"source\":", index);   json_string(index, entry->source);
    fputs(",\"path\":", index);     json_string(index, entry->path);
    return fputs("}", index) >= 0;
}

static bool scan_directory(const char *directory, const char *source, FILE *index,
                           uint32_t *next_id, bool *first) {
    DIR *dir = opendir(directory);
    if (!dir) return false;
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        if (item->d_name[0] == '.') continue;
        char full_path[256];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", directory, item->d_name);
        if (n <= 0 || n >= (int)sizeof(full_path)) continue;

        /* SPIFFS supports slash-containing object names but does not have
         * real directories.  Its opendir() succeeds even for a file name,
         * so only recurse when readdir explicitly reports a directory. */
        if (item->d_type == DT_DIR) {
            if (!scan_directory(full_path, source, index, next_id, first)) {
                closedir(dir);
                return false;
            }
            continue;
        }
        if (!has_sky_extension(item->d_name)) continue;

        library_entry_t entry;
        if (strcasecmp(source, "user") == 0) classify_user_path(full_path, &entry);
        else classify_path(full_path, &entry);
        entry.id = (*next_id)++;
        if (!write_entry(index, &entry, *first)) {
            closedir(dir);
            return false;
        }
        *first = false;
    }
    closedir(dir);
    return true;
}

/* SPIFFS is a flat filesystem on some ESP-IDF versions.  In that mode the
 * staged hierarchy is exposed as slash-containing names at /spiffs rather
 * than as real directories; retain those exact names and index them too. */
static bool scan_flat_spiffs(const char *root, const char *source, FILE *index,
                             uint32_t *next_id, bool *first) {
    DIR *dir = opendir("/spiffs");
    if (!dir) return false;
    struct dirent *item;
    const size_t prefix_len = strlen(root);
    while ((item = readdir(dir)) != NULL) {
        char full_path[256];
        int n = snprintf(full_path, sizeof(full_path), "/spiffs/%s", item->d_name);
        if (n <= 0 || n >= (int)sizeof(full_path) ||
            strncmp(full_path, root, prefix_len) != 0 ||
            full_path[prefix_len] != '/' || !has_sky_extension(full_path)) continue;
        library_entry_t entry;
        if (strcasecmp(source, "user") == 0) classify_user_path(full_path, &entry);
        else classify_path(full_path, &entry);
        entry.id = (*next_id)++;
        if (!write_entry(index, &entry, *first)) { closedir(dir); return false; }
        *first = false;
    }
    closedir(dir);
    return true;
}

bool library_rebuild(void) {
    s_entry_count = -1;
    s_user_count = -1;
    s_favourites_loaded = false;
    FILE *index = fopen(LIBRARY_INDEX_PATH, "wb");
    if (!index) {
        ESP_LOGE(TAG, "Cannot create %s", LIBRARY_INDEX_PATH);
        return false;
    }
    fputs("[\n", index);
    uint32_t next_id = 1;
    bool first = true;
    bool ok = scan_directory(LIBRARY_ROOT, "factory", index, &next_id, &first);
    if (!ok) {
        clearerr(index);
        ok = scan_flat_spiffs(LIBRARY_ROOT, "factory", index, &next_id, &first);
    }
    if (ok) ok = scan_directory(LIBRARY_USER_ROOT, "user", index, &next_id, &first);
    if (!ok) {
        clearerr(index);
        ok = scan_flat_spiffs(LIBRARY_USER_ROOT, "user", index, &next_id, &first);
    }
    if (ok) ok = fputs("\n]\n", index) >= 0 && fflush(index) == 0;
    int close_rc = fclose(index);
    if (!ok || close_rc != 0) {
        ESP_LOGE(TAG, "Library rebuild failed");
        return false;
    }
    s_entry_count = (int)next_id - 1;
    library_prune_favourites();
    ESP_LOGI(TAG, "Indexed %d Skylander library entries", s_entry_count);
    return true;
}

int library_count(void) {
    if (s_entry_count >= 0) return s_entry_count;
    FILE *index = fopen(LIBRARY_INDEX_PATH, "rb");
    if (!index) return 0;
    char line[640];
    int count = 0, users = 0;
    while (fgets(line, sizeof(line), index)) {
        if (strstr(line, "\"id\":")) {
            count++;
            if (strstr(line, "\"source\":\"user\"")) users++;
        }
    }
    fclose(index);
    s_entry_count = count;
    s_user_count = users;
    return count;
}

bool library_init(void) {
    FILE *index = fopen(LIBRARY_INDEX_PATH, "rb");
    if (index) {
        fclose(index);
        library_count();
        return true;
    }
    return library_rebuild();
}

static bool json_field(const char *line, const char *field, char *out, size_t out_size) {
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\":\"", field);
    const char *value = strstr(line, needle);
    if (!value) return false;
    value += strlen(needle);
    size_t used = 0;
    while (*value && *value != '"' && used + 1 < out_size) out[used++] = *value++;
    out[used] = '\0';
    return true;
}

static bool parse_entry(const char *line, library_entry_t *entry) {
    memset(entry, 0, sizeof(*entry));
    copy_field(entry->source, sizeof(entry->source), "factory");
    const char *id = strstr(line, "\"id\":");
    if (!id) return false;
    entry->id = (uint32_t)strtoul(id + 5, NULL, 10);
    bool fields_ok = json_field(line, "name", entry->name, sizeof(entry->name)) &&
           json_field(line, "game", entry->game, sizeof(entry->game)) &&
           json_field(line, "element", entry->element, sizeof(entry->element)) &&
           json_field(line, "type", entry->type, sizeof(entry->type)) &&
           json_field(line, "variant", entry->variant, sizeof(entry->variant)) &&
           json_field(line, "category", entry->category, sizeof(entry->category)) &&
           json_field(line, "path", entry->path, sizeof(entry->path));
    if (fields_ok) json_field(line, "source", entry->source, sizeof(entry->source));
    return fields_ok;
}

bool library_find(uint32_t id, library_entry_t *entry) {
    FILE *index = fopen(LIBRARY_INDEX_PATH, "rb");
    if (!index) return false;
    char line[640];
    bool found = false;
    while (fgets(line, sizeof(line), index)) {
        library_entry_t candidate;
        if (parse_entry(line, &candidate) && candidate.id == id) {
            *entry = candidate;
            found = true;
            break;
        }
    }
    fclose(index);
    return found;
}

static void favourite_key(const library_entry_t *entry, char *out, size_t out_size) {
    snprintf(out, out_size, "%s:%s", entry->source, entry->path);
}

static bool favourite_line_key(const char *line, char *out, size_t out_size) {
    const char *start = strchr(line, '"');
    if (!start) return false;
    start++;
    const char *end = strchr(start, '"');
    if (!end || end == start || (size_t)(end - start) >= out_size) return false;
    memcpy(out, start, (size_t)(end - start));
    out[end - start] = '\0';
    return true;
}

static bool favourite_contains(const library_entry_t *entry) {
    char wanted[300];
    favourite_key(entry, wanted, sizeof(wanted));
    favourite_cache_load();
    uint64_t hash = favourite_hash(wanted);
    for (int i = 0; i < s_favourite_count; i++)
        if (s_favourite_hashes[i] == hash) return true;
    return false;
}

bool library_set_favourite(uint32_t id, bool favourite) {
    library_entry_t entry;
    if (!library_find(id, &entry)) return false;
    char wanted[300], line[340], key[300];
    favourite_key(&entry, wanted, sizeof(wanted));
    FILE *in = fopen(LIBRARY_FAVOURITES_PATH, "rb");
    FILE *out = fopen("/spiffs/favourites.tmp", "wb");
    if (!out) { if (in) fclose(in); return false; }
    fputs("[\n", out);
    bool first = true, already_present = false, ok = true;
    while (in && fgets(line, sizeof(line), in)) {
        if (!favourite_line_key(line, key, sizeof(key))) continue;
        if (strcmp(key, wanted) == 0) {
            already_present = true;
            if (!favourite) continue;
        }
        if (!first) fputs(",\n", out);
        json_string(out, key);
        first = false;
    }
    if (favourite && !already_present) {
        if (!first) fputs(",\n", out);
        json_string(out, wanted);
    }
    if (in) fclose(in);
    ok = fputs("\n]\n", out) >= 0 && fflush(out) == 0 && fclose(out) == 0;
    if (!ok) { remove("/spiffs/favourites.tmp"); return false; }
    remove(LIBRARY_FAVOURITES_PATH);
    bool renamed = rename("/spiffs/favourites.tmp", LIBRARY_FAVOURITES_PATH) == 0;
    s_favourites_loaded = false;
    return renamed;
}

int library_user_count(void) {
    if (s_user_count >= 0) return s_user_count;
    FILE *index = fopen(LIBRARY_INDEX_PATH, "rb");
    if (!index) return 0;
    char line[640]; int count = 0;
    while (fgets(line, sizeof(line), index)) {
        library_entry_t entry;
        if (parse_entry(line, &entry) && strcasecmp(entry.source, "user") == 0) count++;
    }
    fclose(index);
    s_user_count = count;
    return count;
}

static bool favourite_key_exists(const char *key) {
    FILE *index = fopen(LIBRARY_INDEX_PATH, "rb");
    if (!index) return false;
    char line[640], candidate[300]; bool found = false;
    while (fgets(line, sizeof(line), index)) {
        library_entry_t entry;
        if (parse_entry(line, &entry)) {
            favourite_key(&entry, candidate, sizeof(candidate));
            if (strcmp(candidate, key) == 0) { found = true; break; }
        }
    }
    fclose(index);
    return found;
}

static void library_prune_favourites(void) {
    FILE *in = fopen(LIBRARY_FAVOURITES_PATH, "rb");
    if (!in) return;
    FILE *out = fopen("/spiffs/favourites.tmp", "wb");
    if (!out) { fclose(in); return; }
    char line[340], key[300]; bool first = true;
    fputs("[\n", out);
    while (fgets(line, sizeof(line), in)) {
        if (!favourite_line_key(line, key, sizeof(key)) || !favourite_key_exists(key)) continue;
        if (!first) fputs(",\n", out);
        json_string(out, key); first = false;
    }
    fclose(in); fputs("\n]\n", out); fclose(out);
    remove(LIBRARY_FAVOURITES_PATH);
    rename("/spiffs/favourites.tmp", LIBRARY_FAVOURITES_PATH);
}

bool library_delete_user(uint32_t id, int *http_status) {
    if (http_status) *http_status = 500;
    library_entry_t entry;
    if (!library_find(id, &entry)) { if (http_status) *http_status = 404; return false; }
    size_t root_len = strlen(LIBRARY_USER_ROOT);
    if (strcasecmp(entry.source, "user") != 0 ||
        strncmp(entry.path, LIBRARY_USER_ROOT, root_len) != 0 ||
        entry.path[root_len] != '/' || strstr(entry.path, "..")) {
        if (http_status) *http_status = 403;
        return false;
    }
    if (remove(entry.path) != 0) { if (http_status) *http_status = 404; return false; }
    if (!library_rebuild()) { if (http_status) *http_status = 500; return false; }
    if (http_status) *http_status = 200;
    return true;
}

static bool contains_ci(const char *haystack, const char *needle) {
    if (!needle) return true;
    while (isspace((unsigned char)*needle)) needle++;
    size_t n = strlen(needle);
    while (n && isspace((unsigned char)needle[n - 1])) n--;
    if (!n) return true;
    for (; *haystack; haystack++)
        if (strncasecmp(haystack, needle, n) == 0) return true;
    return false;
}

static bool equal_filter(const char *value, const char *filter) {
    if (!filter) return true;
    while (isspace((unsigned char)*filter)) filter++;
    size_t filter_len = strlen(filter);
    while (filter_len && isspace((unsigned char)filter[filter_len - 1])) filter_len--;
    if (!filter_len) return true;
    while (isspace((unsigned char)*value)) value++;
    size_t value_len = strlen(value);
    while (value_len && isspace((unsigned char)value[value_len - 1])) value_len--;
    return value_len == filter_len && strncasecmp(value, filter, value_len) == 0;
}

static bool append_json_string(char **cursor, size_t *left, const char *value) {
    if (*left < 3) return false;
    *(*cursor)++ = '"'; (*left)--;
    while (*value) {
        if (*value == '"' || *value == '\\') {
            if (*left < 2) return false;
            *(*cursor)++ = '\\'; (*left)--;
        }
        if (*left < 2) return false;
        *(*cursor)++ = *value++; (*left)--;
    }
    *(*cursor)++ = '"'; (*left)--;
    **cursor = '\0';
    return true;
}

static bool append_entry_json(char **cursor, size_t *left, const library_entry_t *e) {
    int written = snprintf(*cursor, *left, "{\"id\":%u,\"name\":", (unsigned)e->id);
    if (written < 0 || (size_t)written >= *left) return false;
    *cursor += written; *left -= (size_t)written;
#define LIB_FIELD(KEY, VALUE) do { \
    written = snprintf(*cursor, *left, ",\"" KEY "\":"); \
    if (written < 0 || (size_t)written >= *left) return false; \
    *cursor += written; *left -= (size_t)written; \
    if (!append_json_string(cursor, left, VALUE)) return false; \
} while (0)
    if (!append_json_string(cursor, left, e->name)) return false;
    LIB_FIELD("game", e->game); LIB_FIELD("element", e->element);
    LIB_FIELD("type", e->type); LIB_FIELD("variant", e->variant);
    LIB_FIELD("category", e->category);
    LIB_FIELD("source", e->source);
#undef LIB_FIELD
    written = snprintf(*cursor, *left, ",\"favourite\":%s", e->favourite ? "true" : "false");
    if (written < 0 || (size_t)written >= *left) return false;
    *cursor += written; *left -= (size_t)written;
    if (*left < 2) return false;
    *(*cursor)++ = '}'; (*left)--; **cursor = '\0';
    return true;
}

bool library_query_json(char *out, size_t out_size, const char *search,
                        const char *game, const char *element, const char *type,
                        const char *category, const char *source, bool favourites_only,
                        int page, int limit) {
    if (!out || out_size < 64 || limit < 1 || limit > 50) return false;
    if (page < 1) page = 1;
    FILE *index = fopen(LIBRARY_INDEX_PATH, "rb");
    if (!index) return false;

    char *cursor = out;
    size_t left = out_size;
    int written = snprintf(cursor, left, "{\"page\":%d,\"limit\":%d,\"total\":%d,\"user_total\":%d,\"entries\":[",
                           page, limit, library_count(), library_user_count());
    if (written < 0 || (size_t)written >= left) { fclose(index); return false; }
    cursor += written; left -= (size_t)written;

    char line[640];
    int matched = 0, emitted = 0;
    bool first = true, ok = true;
    while (fgets(line, sizeof(line), index)) {
        library_entry_t entry;
        if (!parse_entry(line, &entry) || !contains_ci(entry.name, search) ||
            !equal_filter(entry.game, game) || !equal_filter(entry.element, element) ||
            !equal_filter(entry.type, type) || !equal_filter(entry.category, category) ||
            !equal_filter(entry.source, source)) continue;
        entry.favourite = favourites_only && favourite_contains(&entry);
        if (favourites_only && !entry.favourite) continue;
        if (matched++ < (page - 1) * limit) continue;
        if (emitted >= limit) continue;
        /* The normal library page needs favourite state only for the cards
         * being returned. Avoid hundreds of SPIFFS opens per request. */
        if (!favourites_only) entry.favourite = favourite_contains(&entry);
        if (!first) {
            if (left < 2) { ok = false; break; }
            *cursor++ = ','; left--; *cursor = '\0';
        }
        if (!append_entry_json(&cursor, &left, &entry)) { ok = false; break; }
        first = false;
        emitted++;
    }
    fclose(index);
    if (!ok || left < 20) return false;
    snprintf(cursor, left, "],\"matches\":%d}", matched);
    return true;
}

#define LIBRARY_MAX_ELEMENTS 11
#define LIBRARY_MAX_TYPES    32

static void add_facet(char values[][32], int *count, int max, const char *value) {
    if (!value[0]) return;
    for (int i = 0; i < *count; i++)
        if (strcasecmp(values[i], value) == 0) return;
    if (*count < max) copy_field(values[(*count)++], 32, value);
}

static bool append_facet_list(char **cursor, size_t *left,
                              char values[][32], int count) {
    for (int i = 0; i < count; i++) {
        if (i) {
            if (*left < 2) return false;
            *(*cursor)++ = ','; (*left)--;
        }
        if (!append_json_string(cursor, left, values[i])) return false;
    }
    return true;
}

bool library_facets_json(char *out, size_t out_size, const char *search,
                         const char *game, const char *element,
                         const char *category, const char *source,
                         bool favourites_only) {
    if (!out || out_size < 128) return false;
    FILE *index = fopen(LIBRARY_INDEX_PATH, "rb");
    if (!index) return false;

    char elements[LIBRARY_MAX_ELEMENTS][32] = {{0}};
    char types[LIBRARY_MAX_TYPES][32] = {{0}};
    int element_count = 0, type_count = 0;
    char line[640];
    while (fgets(line, sizeof(line), index)) {
        library_entry_t entry;
        if (!parse_entry(line, &entry) || !equal_filter(entry.game, game) ||
            !equal_filter(entry.category, category) || !equal_filter(entry.source, source)) continue;
        if (favourites_only && !favourite_contains(&entry)) continue;

        /* Elements describe the selected game; types describe the results
         * after the active element and name filters have been applied. */
        add_facet(elements, &element_count, LIBRARY_MAX_ELEMENTS, entry.element);
        if (equal_filter(entry.element, element) && contains_ci(entry.name, search))
            add_facet(types, &type_count, LIBRARY_MAX_TYPES, entry.type);
    }
    fclose(index);

    char *cursor = out;
    size_t left = out_size;
    int written = snprintf(cursor, left, "{\"elements\":[");
    if (written < 0 || (size_t)written >= left) return false;
    cursor += written; left -= (size_t)written;
    if (!append_facet_list(&cursor, &left, elements, element_count) || left < 12) return false;
    written = snprintf(cursor, left, "],\"types\":[");
    if (written < 0 || (size_t)written >= left) return false;
    cursor += written; left -= (size_t)written;
    if (!append_facet_list(&cursor, &left, types, type_count) || left < 3) return false;
    snprintf(cursor, left, "]}");
    return true;
}
