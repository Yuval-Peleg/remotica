#include "gcode_snippets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

void gcode_snippets_defaults(GcodeSnippets *snippets) {
    memset(snippets, 0, sizeof(*snippets));
}

/* Same contract as printer_profile.c's helper of the same name: only
 * writes `out` when the field is present AND a string, so a malformed or
 * partial body leaves the existing value alone rather than blanking it. */
static void read_string_field(const cJSON *json, const char *field_name, char *out,
                              size_t out_size) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, field_name);
    if (item != NULL && cJSON_IsString(item)) {
        snprintf(out, out_size, "%s", item->valuestring);
    }
}

void gcode_snippets_from_json(GcodeSnippets *snippets, const struct cJSON *json) {
    if (json == NULL) {
        return;
    }
    read_string_field((const cJSON *)json, "startGcode", snippets->start_gcode,
                      sizeof(snippets->start_gcode));
    read_string_field((const cJSON *)json, "endGcode", snippets->end_gcode,
                      sizeof(snippets->end_gcode));
    read_string_field((const cJSON *)json, "writtenFor", snippets->written_for,
                      sizeof(snippets->written_for));
}

struct cJSON *gcode_snippets_to_json(const GcodeSnippets *snippets) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "startGcode", snippets->start_gcode);
    cJSON_AddStringToObject(root, "endGcode", snippets->end_gcode);
    cJSON_AddStringToObject(root, "writtenFor", snippets->written_for);
    return (struct cJSON *)root;
}

void gcode_snippets_load(GcodeSnippets *snippets, const char *path) {
    gcode_snippets_defaults(snippets);

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return; /* first run — nothing saved yet */
    }

    /* Sized for two GCODE_SNIPPET_MAX strings plus JSON overhead and
     * whatever escaping the newlines cost. */
    char buffer[8192];
    size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[bytes_read] = '\0';

    cJSON *json = cJSON_Parse(buffer);
    if (json == NULL) {
        printf("snippets: %s isn't valid JSON — ignoring it and running no start/end "
               "G-code. Re-save from Settings to replace it.\n",
               path);
        fflush(stdout);
        return;
    }

    gcode_snippets_from_json(snippets, (const struct cJSON *)json);
    cJSON_Delete(json);
}

int gcode_snippets_save(const GcodeSnippets *snippets, const char *path) {
    cJSON *json = (cJSON *)gcode_snippets_to_json(snippets);
    char *text = cJSON_Print(json);
    cJSON_Delete(json);

    if (text == NULL) {
        return -1;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        free(text);
        return -1;
    }

    fputs(text, file);
    fclose(file);
    free(text);
    return 0;
}
