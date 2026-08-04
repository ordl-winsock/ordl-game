/*
 * TOML parser - simplified, handles config needs only.
 * Pure C23, no external libraries.
 */

#ifndef ORDL_INFERCLI_TOML_H
#define ORDL_INFERCLI_TOML_H

#include "ordl_infercli.h"
#include "ordl_infercli_arena.h"

typedef struct toml_value toml_value_t;

struct toml_value {
    enum { TOML_STRING, TOML_INT, TOML_BOOL, TOML_ARRAY, TOML_TABLE } type;
    union {
        char *s;
        int64_t i;
        bool b;
        struct {
            toml_value_t **items;
            size_t count;
        } arr;
        struct {
            char **keys;
            toml_value_t **vals;
            size_t count;
        } tbl;
    };
};

/* Parse TOML text into arena-backed table. Returns root table or NULL. */
toml_value_t *toml_parse(const char *text, arena_t *arena);

/* Get value from table by key */
const toml_value_t *toml_table_get(const toml_value_t *tbl, const char *key);

/* Get string value */
const char *toml_string(const toml_value_t *v);

/* Get nested table value from table path (e.g. "mcp_servers.mock") */
const toml_value_t *toml_table_path(const toml_value_t *root, const char *path);

#endif
