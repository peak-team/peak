#ifndef PEAK_TARGET_SELECTOR_SYNTAX_H
#define PEAK_TARGET_SELECTOR_SYNTAX_H

#include <stdbool.h>

/*
 * Finds a delimiter outside a C++ selector's nested declarators.  The parser
 * intentionally has no resolver dependency so PEAK_TARGET and
 * PEAK_TARGET_FILE share the exact same lexical rules.
 *
 * With recover_nested set, an unclosed declarator returns its first nested
 * delimiter.  This preserves the target-list parser's long-standing ability
 * to keep later list entries usable after one malformed entry.
 */
bool peak_target_selector_next_delimiter(const char* text,
                                         char delimiter,
                                         bool recover_nested,
                                         const char** delimiter_out);

/** Returns whether @p text has a top-level legacy @c +0x offset suffix. */
bool peak_target_selector_has_top_level_offset(const char* text);

#endif /* PEAK_TARGET_SELECTOR_SYNTAX_H */
