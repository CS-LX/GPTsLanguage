#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "grammar.h"

extern const char prose_program[];
extern const size_t prose_size;

static bool consume(const char **cursor, const char *end, const char *form)
{
    size_t length = strlen(form);
    if ((size_t)(end - *cursor) < length || memcmp(*cursor, form, length) != 0)
        return false;
    *cursor += length;
    return true;
}

/* The export table gives this interpreter the source function's public name. */
uint32_t prose_entry(uint32_t first, uint32_t second)
{
    const char *cursor = prose_program;
    const char *end = cursor + prose_size;
    const uint32_t args[] = {first, second};
    uint32_t total;
    if (consume(&cursor, end, start_arg[0]))
        total = first;
    else if (consume(&cursor, end, start_arg[1]))
        total = second;
    else
        abort();

    if (!consume(&cursor, end, "\n\n"))
        abort();
    for (unsigned i = 0; i < 2; ++i) {
        if (consume(&cursor, end, add_arg[i])) {
            total += args[i];
            break;
        }
        if (consume(&cursor, end, sub_arg[i])) {
            total -= args[i];
            break;
        }
        if (i == 1)
            abort();
    }
    if (!consume(&cursor, end, "\n\n") || !consume(&cursor, end, finish) || cursor != end)
        abort();
    return total;
}
