#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIBRARY_ROOT       "/spiffs/skylandersDumps"
#define LIBRARY_USER_ROOT  "/spiffs/userDumps"
#define LIBRARY_INDEX_PATH "/spiffs/skylander_index.json"
#define LIBRARY_FAVOURITES_PATH "/spiffs/favourites.json"

typedef struct {
    uint32_t id;
    char name[96];
    char game[32];
    char element[16];
    char type[32];
    char variant[96];
    char category[24];
    char source[8];
    char path[256];
    bool favourite;
} library_entry_t;

/* Builds the on-flash metadata index without retaining dump data in RAM. */
bool library_init(void);
bool library_rebuild(void);
int  library_count(void);

/* Streams matching index records into a caller-owned JSON buffer. */
bool library_query_json(char *out, size_t out_size, const char *search,
                        const char *game, const char *element,
                        const char *type, const char *category, const char *source,
                        bool favourites_only,
                        int page, int limit);

/* Returns the element/type choices represented by the existing metadata
 * index. This is deliberately separate from filesystem indexing. */
bool library_facets_json(char *out, size_t out_size, const char *search,
                         const char *game, const char *element,
                         const char *category, const char *source,
                         bool favourites_only);

/* Resolves an index id to its exact SPIFFS path. */
bool library_find(uint32_t id, library_entry_t *entry);
int  library_user_count(void);
bool library_set_favourite(uint32_t id, bool favourite);
bool library_delete_user(uint32_t id, int *http_status);
