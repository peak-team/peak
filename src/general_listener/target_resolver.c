#include "internal/target_resolver.h"

#include "utils/cxx_utils.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    gchar* module;
    gchar* symbol;
    guintptr offset;
} PeakTargetSelector;

typedef struct {
    const PeakTargetSelector* selector;
    const char* module_path;
    const char* current_module;
    guintptr current_module_start;
    guintptr current_module_end;
    gboolean current_module_range_valid;
    GPtrArray* candidates;
    gboolean cxx_selector;
} PeakTargetCollectContext;

enum {
    PEAK_TARGET_MATCH_EXACT = 0,
    PEAK_TARGET_MATCH_FULL = 1,
    PEAK_TARGET_MATCH_LEGACY = 2,
};

static void
peak_target_symbol_candidate_free(gpointer data)
{
    PeakTargetSymbolCandidate* candidate = data;

    if (candidate == NULL) {
        return;
    }
    g_free(candidate->module);
    g_free(candidate->mangled);
    g_free(candidate->demangled);
    g_free(candidate);
}

void
peak_target_resolution_clear(PeakTargetResolution* resolution)
{
    if (resolution == NULL) {
        return;
    }
    g_clear_pointer(&resolution->candidates, g_ptr_array_unref);
}

static gboolean
peak_target_operator_token_ends_at(const char* text, const char* cursor)
{
    while (cursor > text && g_ascii_isspace(cursor[-1])) {
        cursor--;
    }
    return cursor - text >= 8 &&
           memcmp(cursor - 8, "operator", 8) == 0 &&
           (cursor - 8 == text ||
            (!isalnum((unsigned char)cursor[-9]) && cursor[-9] != '_'));
}

static gboolean
peak_target_operator_punctuation_at(const char* text, const char* cursor)
{
    static const char* const tokens[] = {
        "<=>", "->*", "<<=", ">>=", "<<", ">>", "<=", ">=", "==", "!=",
        "++", "--", "&&", "||", "+=", "-=", "*=", "/=", "%=", "^=", "&=",
        "|=", "->", "<", ">", "+", "-", "*", "/", "%", "^", "&", "|", "~",
        "!", "=", ",",
    };

    for (const char* word = text; word < cursor; word++) {
        const char* token;

        if (strncmp(word, "operator", 8) != 0 ||
            (word > text && (g_ascii_isalnum(word[-1]) || word[-1] == '_'))) {
            continue;
        }
        token = word + 8;
        while (g_ascii_isspace(*token)) {
            token++;
        }
        for (size_t i = 0; i < G_N_ELEMENTS(tokens); i++) {
            size_t length = strlen(tokens[i]);

            if (strncmp(token, tokens[i], length) == 0 &&
                cursor >= token && cursor < token + length) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

typedef struct {
    gboolean grouping;
    char closer;
    size_t id;
} PeakTargetParseFrame;

typedef struct {
    guint frame;
    gboolean operator_continuation;
} PeakTargetAngleDelimiter;

static gboolean
peak_target_preceding_keyword(const char* text,
                              const char* cursor,
                              const char* keyword)
{
    const char* end = cursor;
    const char* begin;
    size_t length = strlen(keyword);

    while (end > text && g_ascii_isspace(end[-1])) {
        end--;
    }
    if ((size_t)(end - text) < length ||
        memcmp(end - length, keyword, length) != 0) {
        return FALSE;
    }
    begin = end - length;
    return begin == text ||
           (!g_ascii_isalnum(begin[-1]) && begin[-1] != '_');
}

static gboolean
peak_target_parenthesis_is_grouping(const char* text, const char* cursor)
{
    while (cursor > text && g_ascii_isspace(cursor[-1])) {
        cursor--;
    }
    if (cursor == text) {
        return TRUE;
    }
    if (peak_target_preceding_keyword(text, cursor, "decltype") ||
        peak_target_preceding_keyword(text, cursor, "noexcept") ||
        peak_target_preceding_keyword(text, cursor, "sizeof") ||
        peak_target_preceding_keyword(text, cursor, "alignof") ||
        peak_target_preceding_keyword(text, cursor, "typeid")) {
        return TRUE;
    }
    return strchr("<[{,(+-*/%&|^~!=?>", cursor[-1]) != NULL;
}

static gboolean
peak_target_frame_has_angle(const PeakTargetAngleDelimiter* angles,
                            size_t angle_count,
                            size_t frame)
{
    for (size_t i = angle_count; i > 0; i--) {
        if (angles[i - 1].frame == frame) {
            return TRUE;
        }
    }
    return FALSE;
}

static void
peak_target_drop_frame_angles(const PeakTargetAngleDelimiter* angles,
                              size_t* angle_count,
                              size_t frame)
{
    while (*angle_count > 0 && angles[*angle_count - 1].frame == frame) {
        (*angle_count)--;
    }
}

static gboolean
peak_target_close_frame_angle(const PeakTargetAngleDelimiter* angles,
                              size_t* angle_count,
                              size_t frame)
{
    if (*angle_count > 0 && angles[*angle_count - 1].frame == frame) {
        (*angle_count)--;
        return TRUE;
    }
    return FALSE;
}

static gboolean
peak_target_find_module_separator(const char* text, const char** separator_out)
{
    const char* separator = NULL;
    PeakTargetParseFrame root = { FALSE, '\0', 0 };
    size_t length = strlen(text);
    size_t capacity;
    PeakTargetParseFrame* frames = NULL;
    PeakTargetAngleDelimiter* angles = NULL;
    size_t frame_count = 0;
    size_t angle_count = 0;
    size_t next_frame_id = 1;
    gboolean valid = FALSE;

    if (length == SIZE_MAX ||
        length + 1 > SIZE_MAX / sizeof(*frames) ||
        length + 1 > SIZE_MAX / sizeof(*angles)) {
        return FALSE;
    }
    capacity = length + 1;
    frames = malloc(capacity * sizeof(*frames));
    angles = malloc(capacity * sizeof(*angles));
    if (frames == NULL || angles == NULL) {
        goto done;
    }
    frames[frame_count++] = root;
    for (const char* cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '(' || *cursor == '[' || *cursor == '{') {
            PeakTargetParseFrame frame = {
                *cursor == '(' ? peak_target_parenthesis_is_grouping(text, cursor) : TRUE,
                *cursor == '(' ? ')' : (*cursor == '[' ? ']' : '}'),
                next_frame_id++
            };

            frames[frame_count++] = frame;
        } else if (*cursor == ')' || *cursor == ']' || *cursor == '}') {
            size_t frame;
            PeakTargetParseFrame current;

            if (frame_count == 1) {
                goto done;
            }
            frame = frame_count - 1;
            current = frames[frame];
            if (*cursor != current.closer) {
                goto done;
            }
            if (peak_target_frame_has_angle(angles, angle_count, current.id)) {
                if (!current.grouping) {
                    goto done;
                }
                peak_target_drop_frame_angles(angles, &angle_count, current.id);
            }
            frame_count = frame;
        } else if (*cursor == '<' &&
                   !peak_target_operator_punctuation_at(text, cursor) &&
                   cursor[1] != '=') {
            PeakTargetAngleDelimiter angle = { frames[frame_count - 1].id };

            angles[angle_count++] = angle;
        } else if (*cursor == '>' &&
                   !peak_target_operator_punctuation_at(text, cursor)) {
            PeakTargetParseFrame current = frames[frame_count - 1];

            if (cursor > text && cursor[-1] == '=') {
                goto done;
            }
            if (cursor[1] != '=' &&
                !peak_target_close_frame_angle(angles, &angle_count, current.id) &&
                !current.grouping) {
                goto done;
            }
        } else if (*cursor == '!' && frame_count == 1 && angle_count == 0 &&
                   !peak_target_operator_token_ends_at(text, cursor)) {
            if (separator != NULL) {
                goto done;
            }
            separator = cursor;
        }
    }
    if (frame_count != 1 || angle_count != 0) {
        goto done;
    }
    valid = TRUE;

done:
    free(angles);
    free(frames);
    if (!valid) {
        return FALSE;
    }
    *separator_out = separator;
    return TRUE;
}

static gboolean
peak_target_parse_offset(char* symbol, guintptr* offset_out)
{
    char* plus = strrchr(symbol, '+');
    char* marker;
    char* end = NULL;
    unsigned long long value;

    *offset_out = 0;
    if (strstr(symbol, "+0X") != NULL) {
        return FALSE;
    }
    marker = strstr(symbol, "+0x");
    if (plus == NULL || strncmp(plus, "+0x", 3) != 0) {
        if (marker != NULL) {
            return FALSE;
        }
        return TRUE;
    }
    if (marker != plus || plus[3] == '\0') {
        return FALSE;
    }
    for (char* cursor = plus + 3; *cursor != '\0'; cursor++) {
        if (!g_ascii_isxdigit(*cursor)) {
            return FALSE;
        }
    }
    errno = 0;
    value = strtoull(plus + 3, &end, 16);
    if (errno == ERANGE || end == plus + 3 || *end != '\0' ||
        value > UINTPTR_MAX) {
        return FALSE;
    }
    *plus = '\0';
    if (symbol[0] == '\0') {
        return FALSE;
    }
    *offset_out = (guintptr)value;
    return TRUE;
}

static gboolean
peak_target_parse_selector(const char* input, PeakTargetSelector* selector)
{
    const char* bang;
    const char* symbol;

    memset(selector, 0, sizeof(*selector));
    if (input == NULL || input[0] == '\0') {
        return FALSE;
    }
    if (!peak_target_find_module_separator(input, &bang)) {
        return FALSE;
    }
    symbol = input;
    if (bang != NULL) {
        if (bang == input || bang[1] == '\0') {
            return FALSE;
        }
        selector->module = g_strndup(input, (gsize)(bang - input));
        symbol = bang + 1;
    }
    selector->symbol = g_strdup(symbol);
    if (selector->symbol == NULL ||
        !peak_target_parse_offset(selector->symbol, &selector->offset)) {
        g_free(selector->module);
        g_free(selector->symbol);
        memset(selector, 0, sizeof(*selector));
        return FALSE;
    }
    return TRUE;
}

static void
peak_target_selector_clear(PeakTargetSelector* selector)
{
    g_free(selector->module);
    g_free(selector->symbol);
}

gboolean
peak_target_resolver_validate_selector(const char* selector_text)
{
    PeakTargetSelector selector;
    gboolean valid = peak_target_parse_selector(selector_text, &selector);

    if (valid) {
        peak_target_selector_clear(&selector);
    }
    return valid;
}

gboolean
peak_target_resolver_dup_selector_module(const char* selector_text,
                                         gchar** module_out)
{
    PeakTargetSelector selector;

    if (module_out == NULL || !peak_target_parse_selector(selector_text, &selector)) {
        return FALSE;
    }
    *module_out = selector.module;
    selector.module = NULL;
    peak_target_selector_clear(&selector);
    return TRUE;
}

gboolean
peak_target_resolver_module_matches(const char* requested,
                                    const char* module_path)
{
    const char* basename;
    char canonical_requested[PATH_MAX];
    char canonical_module[PATH_MAX];

    if (requested == NULL) {
        return TRUE;
    }
    if (module_path == NULL) {
        return FALSE;
    }
    if (strcmp(requested, module_path) == 0) {
        return TRUE;
    }
    if (strchr(requested, G_DIR_SEPARATOR) != NULL) {
        return realpath(requested, canonical_requested) != NULL &&
               realpath(module_path, canonical_module) != NULL &&
               strcmp(canonical_requested, canonical_module) == 0;
    }
    basename = strrchr(module_path, G_DIR_SEPARATOR);
    return strcmp(requested, basename != NULL ? basename + 1 : module_path) == 0;
}

static gchar*
peak_target_without_space(const char* text)
{
    GString* normalized = g_string_sized_new(strlen(text));

    for (const unsigned char* p = (const unsigned char*)text; *p != '\0'; p++) {
        if (!g_ascii_isspace(*p)) {
            g_string_append_c(normalized, (gchar)*p);
        }
    }
    return g_string_free(normalized, FALSE);
}

static gboolean
peak_target_equal_normalized(const char* left, const char* right)
{
    gchar* normalized_left = peak_target_without_space(left);
    gchar* normalized_right = peak_target_without_space(right);
    gboolean equal = strcmp(normalized_left, normalized_right) == 0;

    g_free(normalized_left);
    g_free(normalized_right);
    return equal;
}

static gboolean
peak_target_is_artificial_prefix(const char* demangled)
{
    const char* clone = g_strrstr(demangled, " [clone ");

    return g_str_has_prefix(demangled, "non-virtual thunk to ") ||
           g_str_has_prefix(demangled, "virtual thunk to ") ||
           g_str_has_prefix(demangled, "covariant return thunk to ") ||
           g_str_has_prefix(demangled, "transaction clone for ") ||
           g_str_has_prefix(demangled, "non-transaction clone for ") ||
           (clone != NULL && clone[8] != '\0' &&
            strchr(clone + 8, ']') == demangled + strlen(demangled) - 1);
}

static const char*
peak_target_trim_end(const char* begin, const char* end)
{
    while (end > begin && g_ascii_isspace(end[-1])) {
        end--;
    }
    return end;
}

static const char*
peak_target_final_parameter_open(const char* selector)
{
    const char* close = strrchr(selector, ')');
    unsigned int depth = 1;

    if (close == NULL) {
        return NULL;
    }
    for (const char* cursor = close; cursor-- > selector;) {
        if (*cursor == ')') {
            depth++;
        } else if (*cursor == '(' && --depth == 0) {
            return cursor;
        }
    }
    return NULL;
}

static const char*
peak_target_strip_template_suffix(const char* begin, const char* end)
{
    unsigned int depth = 0;

    end = peak_target_trim_end(begin, end);
    if (end == begin || end[-1] != '>') {
        return end;
    }
    for (const char* cursor = end; cursor-- > begin;) {
        if (*cursor == '>') {
            depth++;
        } else if (*cursor == '<' && --depth == 0) {
            return peak_target_trim_end(begin, cursor);
        }
    }
    return end;
}

static gboolean
peak_target_selector_has_function_id(const char* begin, const char* end)
{
    end = peak_target_strip_template_suffix(begin, end);
    if (end > begin && (g_ascii_isalnum(end[-1]) || end[-1] == '_')) {
        return TRUE;
    }
    for (const char* cursor = begin; cursor + 8 <= end; cursor++) {
        if (memcmp(cursor, "operator", 8) == 0 &&
            (cursor == begin ||
             (!g_ascii_isalnum(cursor[-1]) && cursor[-1] != '_'))) {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
peak_target_selector_is_complete_signature(const char* selector)
{
    const char* parameter_open = peak_target_final_parameter_open(selector);

    return parameter_open != NULL &&
           peak_target_selector_has_function_id(selector, parameter_open);
}

static const char*
peak_target_last_top_level_space(const char* begin, const char* end)
{
    const char* result = NULL;
    unsigned int parentheses = 0;
    unsigned int brackets = 0;
    unsigned int braces = 0;
    unsigned int templates = 0;

    for (const char* cursor = begin; cursor < end; cursor++) {
        if (*cursor == '(') {
            parentheses++;
        } else if (*cursor == ')' && parentheses > 0) {
            parentheses--;
        } else if (*cursor == '[') {
            brackets++;
        } else if (*cursor == ']' && brackets > 0) {
            brackets--;
        } else if (*cursor == '{') {
            braces++;
        } else if (*cursor == '}' && braces > 0) {
            braces--;
        } else if (*cursor == '<') {
            templates++;
        } else if (*cursor == '>' && templates > 0) {
            templates--;
        } else if (g_ascii_isspace(*cursor) && parentheses == 0 &&
                   brackets == 0 && braces == 0 && templates == 0) {
            result = cursor;
        }
    }
    return result;
}

static const char*
peak_target_last_top_level_operator(const char* begin, const char* end)
{
    const char* result = NULL;
    unsigned int parentheses = 0;
    unsigned int brackets = 0;
    unsigned int braces = 0;
    unsigned int templates = 0;

    for (const char* cursor = begin; cursor + 8 <= end; cursor++) {
        if (*cursor == '(') {
            parentheses++;
        } else if (*cursor == ')' && parentheses > 0) {
            parentheses--;
        } else if (*cursor == '[') {
            brackets++;
        } else if (*cursor == ']' && brackets > 0) {
            brackets--;
        } else if (*cursor == '{') {
            braces++;
        } else if (*cursor == '}' && braces > 0) {
            braces--;
        } else if (*cursor == '<') {
            templates++;
        } else if (*cursor == '>' && templates > 0) {
            templates--;
        }
        if (parentheses == 0 && brackets == 0 && braces == 0 && templates == 0 &&
            memcmp(cursor, "operator", 8) == 0 &&
            (cursor == begin ||
             (!g_ascii_isalnum(cursor[-1]) && cursor[-1] != '_')) &&
            (cursor + 8 == end ||
             (!g_ascii_isalnum(cursor[8]) && cursor[8] != '_'))) {
            result = cursor;
        }
    }
    return result;
}

static const char*
peak_target_demangled_function_start(const char* demangled)
{
    const char* parameter_open = peak_target_final_parameter_open(demangled);
    const char* operator_word;
    const char* boundary;
    const char* space;

    if (parameter_open == NULL) {
        return demangled;
    }
    operator_word = peak_target_last_top_level_operator(demangled, parameter_open);
    boundary = operator_word != NULL ? operator_word : parameter_open;
    space = peak_target_last_top_level_space(demangled, boundary);
    if (space == NULL) {
        return demangled;
    }
    return space + 1;
}

static gboolean
peak_target_declarator_boundary(const char* begin, const char* cursor)
{
    return cursor == begin || g_ascii_isspace(cursor[-1]) ||
           (cursor > begin && cursor[-1] == '*' &&
            (cursor - begin == 1 || cursor[-2] == '(' || cursor[-2] == ':')) ||
           (cursor > begin && cursor[-1] == '&' &&
            (cursor - begin == 1 || cursor[-2] == '('));
}

static const char*
peak_target_normalized_prefix_end(const char* normalized_selector,
                                  const char* candidate)
{
    for (const char* selector = normalized_selector; *selector != '\0'; selector++) {
        while (g_ascii_isspace(*candidate)) {
            candidate++;
        }
        if (*candidate != *selector) {
            return NULL;
        }
        candidate++;
    }
    return candidate;
}

static gboolean
peak_target_function_signature_ends_at(const char* end)
{
    while (g_ascii_isspace(*end)) {
        end++;
    }
    /* A closing declarator parenthesis starts the return type continuation of
     * a function-pointer/array return, not a cv/ref/exception qualifier of
     * the exported function itself. */
    return *end == '\0' || *end == ')';
}

static gboolean
peak_target_parenthesis_introduces_pointer(const char* cursor)
{
    const char* lookahead = cursor + 1;

    while (g_ascii_isspace(*lookahead)) {
        lookahead++;
    }
    if (*lookahead == '*' || *lookahead == '&') {
        return TRUE;
    }
    for (; *lookahead != '\0' && *lookahead != '(' && *lookahead != ')';
         lookahead++) {
        if (lookahead[0] == ':' && lookahead[1] == ':' && lookahead[2] == '*') {
            return TRUE;
        }
    }
    return FALSE;
}

typedef struct {
    PeakTargetParseFrame frame;
    gboolean pointer_declarator;
    /* Once an operator function-id starts, no later token in this declarator
     * frame may become a separate name-only anchor. */
    gboolean operator_continuation;
} PeakTargetDemangledFrame;

static gboolean
peak_target_operator_continuation_active(
    const PeakTargetDemangledFrame* frames,
    size_t frame_count,
    const PeakTargetAngleDelimiter* angles,
    size_t angle_count)
{
    if (angle_count > 0 &&
        angles[angle_count - 1].frame == frames[frame_count - 1].frame.id) {
        return angles[angle_count - 1].operator_continuation;
    }
    return frames[frame_count - 1].operator_continuation;
}

static void
peak_target_set_operator_continuation(PeakTargetDemangledFrame* frames,
                                      size_t frame_count,
                                      PeakTargetAngleDelimiter* angles,
                                      size_t angle_count)
{
    if (angle_count > 0 &&
        angles[angle_count - 1].frame == frames[frame_count - 1].frame.id) {
        angles[angle_count - 1].operator_continuation = TRUE;
    } else {
        frames[frame_count - 1].operator_continuation = TRUE;
    }
}

static gboolean
peak_target_operator_word_at(const char* text, const char* cursor)
{
    return strncmp(cursor, "operator", 8) == 0 &&
           (cursor == text ||
            (!g_ascii_isalnum(cursor[-1]) && cursor[-1] != '_')) &&
           (!g_ascii_isalnum(cursor[8]) && cursor[8] != '_');
}

static gboolean
peak_target_demangled_contains_name_only_selector(const char* selector,
                                                  const char* demangled)
{
    gchar* normalized_selector = peak_target_without_space(selector);
    size_t length = strlen(demangled);
    PeakTargetDemangledFrame* frames = calloc(length + 1, sizeof(*frames));
    PeakTargetAngleDelimiter* angles = calloc(length + 1, sizeof(*angles));
    size_t frame_count = 1;
    size_t angle_count = 0;
    size_t next_frame_id = 1;
    gboolean matched = FALSE;

    if (frames == NULL || angles == NULL) {
        free(angles);
        free(frames);
        g_free(normalized_selector);
        return FALSE;
    }
    frames[0].frame.closer = '\0';
    for (const char* cursor = demangled; *cursor != '\0'; cursor++) {
        const char* end;
        gboolean function_declarator = angle_count == 0;

        for (size_t i = 1; function_declarator && i < frame_count; i++) {
            function_declarator = frames[i].pointer_declarator;
        }

        if (function_declarator &&
            peak_target_declarator_boundary(demangled, cursor) &&
            !peak_target_operator_continuation_active(
                frames, frame_count, angles, angle_count) &&
            (end = peak_target_normalized_prefix_end(normalized_selector, cursor)) != NULL &&
            peak_target_function_signature_ends_at(end)) {
            matched = TRUE;
            break;
        }
        if (*cursor == '(' || *cursor == '[' || *cursor == '{') {
            PeakTargetDemangledFrame frame = {
                .frame = {
                    .grouping = *cursor == '('
                        ? peak_target_parenthesis_is_grouping(demangled, cursor)
                        : TRUE,
                    .closer = *cursor == '(' ? ')' : (*cursor == '[' ? ']' : '}'),
                    .id = next_frame_id++,
                },
                .pointer_declarator = *cursor == '(' &&
                    peak_target_parenthesis_introduces_pointer(cursor),
            };

            frames[frame_count++] = frame;
        } else if (*cursor == ')' || *cursor == ']' || *cursor == '}') {
            PeakTargetDemangledFrame* frame;

            if (frame_count == 1) {
                continue;
            }
            frame = &frames[frame_count - 1];
            if (*cursor != frame->frame.closer) {
                continue;
            }
            if (peak_target_frame_has_angle(angles, angle_count, frame->frame.id)) {
                peak_target_drop_frame_angles(angles, &angle_count, frame->frame.id);
            }
            frame_count--;
        } else if (*cursor == '<') {
            if (!peak_target_operator_punctuation_at(demangled, cursor)) {
                PeakTargetAngleDelimiter angle = {
                    frames[frame_count - 1].frame.id
                };

                angles[angle_count++] = angle;
            }
        } else if (*cursor == '>' &&
                   !peak_target_operator_punctuation_at(demangled, cursor)) {
            (void)peak_target_close_frame_angle(
                angles, &angle_count, frames[frame_count - 1].frame.id);
        }
        if (peak_target_operator_word_at(demangled, cursor)) {
            peak_target_set_operator_continuation(
                frames, frame_count, angles, angle_count);
        }
    }
    free(angles);
    free(frames);
    g_free(normalized_selector);
    return matched;
}

gboolean
peak_target_resolver_full_signature_matches(const char* selector,
                                            const char* demangled)
{
    if (peak_target_equal_normalized(selector, demangled)) {
        return TRUE;
    }
    if (peak_target_is_artificial_prefix(demangled)) {
        return FALSE;
    }
    if (!peak_target_selector_is_complete_signature(selector)) {
        return FALSE;
    }
    if (peak_target_demangled_function_start(selector) != selector) {
        return FALSE;
    }
    return peak_target_demangled_contains_name_only_selector(selector, demangled);
}

static char*
peak_target_without_final_parameters(const char* demangled)
{
    const char* close = strrchr(demangled, ')');
    unsigned int depth = 1;

    if (close == NULL) {
        return g_strdup(demangled);
    }
    for (const char* cursor = close; cursor-- > demangled;) {
        if (*cursor == ')') {
            depth++;
        } else if (*cursor == '(' && --depth == 0) {
            return g_strndup(demangled, (gsize)(cursor - demangled));
        }
    }
    return g_strdup(demangled);
}

static gboolean
peak_target_is_cxx_selector(const char* symbol)
{
    return g_str_has_prefix(symbol, "_Z") || strstr(symbol, "::") != NULL ||
           strchr(symbol, '(') != NULL || strchr(symbol, '<') != NULL ||
           strstr(symbol, "operator") != NULL;
}

static gboolean
peak_target_legacy_short_matches(const char* selector, const char* demangled)
{
    char* short_name;
    const char* parameters;
    gboolean matched;

    if (peak_target_is_artificial_prefix(demangled)) {
        return FALSE;
    }
    /* Legacy qualified names may omit the final function parameter list, but
     * must retain their full namespace/class spelling. */
    if (strstr(selector, "::") != NULL) {
        char* qualified = peak_target_without_final_parameters(demangled);
        parameters = strrchr(qualified, ' ');
        if (parameters != NULL) {
            memmove(qualified, parameters + 1, strlen(parameters + 1) + 1);
        }
        matched = peak_target_equal_normalized(selector, qualified);
        g_free(qualified);
        return matched;
    }
    short_name = extract_function_name(demangled);
    matched = short_name != NULL && strcmp(selector, short_name) == 0;
    free(short_name);
    return matched;
}

static int
peak_target_symbol_matches(const PeakTargetCollectContext* context,
                           const char* mangled,
                           const char* demangled)
{
    const char* symbol = context->selector->symbol;

    if (strcmp(symbol, mangled) == 0) {
        return PEAK_TARGET_MATCH_EXACT;
    }
    if (!context->cxx_selector || demangled == NULL) {
        return -1;
    }
    if (peak_target_resolver_full_signature_matches(symbol, demangled)) {
        return PEAK_TARGET_MATCH_FULL;
    }
    return peak_target_legacy_short_matches(symbol, demangled)
        ? PEAK_TARGET_MATCH_LEGACY : -1;
}

static gboolean
peak_target_collect_symbol(const GumSymbolDetails* details, gpointer user_data)
{
    PeakTargetCollectContext* context = user_data;
    const char* mangled;
    char* demangled;
    PeakTargetSymbolCandidate* candidate;
    int tier;

    if (details->type != GUM_SYMBOL_FUNCTION || details->address == 0 ||
        details->name == NULL) {
        return TRUE;
    }
    mangled = details->name;
    demangled = cxa_demangle(mangled);
    tier = demangled != NULL
        ? peak_target_symbol_matches(context, mangled, demangled) : -1;
    if (tier < 0) {
        free(demangled);
        return TRUE;
    }
    if (context->selector->offset > 0 && details->size > 0 &&
        context->selector->offset >= (guintptr)details->size) {
        free(demangled);
        return TRUE;
    }
    if (UINTPTR_MAX - (guintptr)details->address < context->selector->offset) {
        free(demangled);
        return TRUE;
    }
    if (context->selector->offset > 0 &&
        (!context->current_module_range_valid ||
         (guintptr)details->address + context->selector->offset <
             context->current_module_start ||
         (guintptr)details->address + context->selector->offset >=
             context->current_module_end)) {
        free(demangled);
        return TRUE;
    }
    candidate = g_new0(PeakTargetSymbolCandidate, 1);
    candidate->address = (gpointer)((guintptr)details->address +
                                    context->selector->offset);
    candidate->symbol_address = (gpointer)details->address;
    candidate->size = details->size > 0 ? (gsize)details->size : 0;
    candidate->module = g_strdup(context->current_module);
    candidate->mangled = g_strdup(mangled);
    candidate->demangled = g_strdup(demangled);
    candidate->match_tier = (unsigned int)tier;
    for (gsize i = 0; i < context->candidates->len; i++) {
        PeakTargetSymbolCandidate* existing =
            g_ptr_array_index(context->candidates, i);
        if (existing->address == candidate->address &&
            g_strcmp0(existing->module, candidate->module) == 0 &&
            g_strcmp0(existing->mangled, candidate->mangled) == 0) {
            peak_target_symbol_candidate_free(candidate);
            free(demangled);
            return TRUE;
        }
    }
    g_ptr_array_add(context->candidates, candidate);
    free(demangled);
    return TRUE;
}

static gboolean
peak_target_collect_module(GumModule* module, gpointer user_data)
{
    PeakTargetCollectContext* context = user_data;
    const char* path = gum_module_get_path(module);
    const GumMemoryRange* range;

    if (!peak_target_resolver_module_matches(context->selector->module, path) ||
        !peak_target_resolver_module_matches(context->module_path, path)) {
        return TRUE;
    }
    context->current_module = path != NULL ? path : "<unknown>";
    range = gum_module_get_range(module);
    context->current_module_range_valid =
        range != NULL && range->size > 0 &&
        UINTPTR_MAX - (guintptr)range->base_address >= range->size;
    if (context->current_module_range_valid) {
        context->current_module_start = (guintptr)range->base_address;
        context->current_module_end = context->current_module_start + range->size;
    }
    gum_module_enumerate_symbols(module, peak_target_collect_symbol, context);
    context->current_module = NULL;
    context->current_module_range_valid = FALSE;
    return TRUE;
}

static gint
peak_target_candidate_compare(gconstpointer left, gconstpointer right)
{
    const PeakTargetSymbolCandidate* a = *(PeakTargetSymbolCandidate* const*)left;
    const PeakTargetSymbolCandidate* b = *(PeakTargetSymbolCandidate* const*)right;
    int comparison = g_strcmp0(a->module, b->module);

    if (comparison != 0) return comparison;
    comparison = g_strcmp0(a->mangled, b->mangled);
    if (comparison != 0) return comparison;
    if ((guintptr)a->address < (guintptr)b->address) return -1;
    if ((guintptr)a->address > (guintptr)b->address) return 1;
    return 0;
}

PeakTargetResolveResult
peak_target_resolver_resolve(const char* selector_text,
                             const char* module_path,
                             gboolean allow_legacy_short,
                             PeakTargetResolution* resolution)
{
    PeakTargetSelector selector;
    PeakTargetCollectContext context;

    if (resolution == NULL) {
        return PEAK_TARGET_RESOLVE_INVALID;
    }
    peak_target_resolution_clear(resolution);
    if (!peak_target_parse_selector(selector_text, &selector)) {
        return PEAK_TARGET_RESOLVE_INVALID;
    }
    resolution->candidates = g_ptr_array_new_with_free_func(
        peak_target_symbol_candidate_free);
    context = (PeakTargetCollectContext){
        .selector = &selector,
        .module_path = module_path,
        .candidates = resolution->candidates,
        .cxx_selector = allow_legacy_short ||
                        peak_target_is_cxx_selector(selector.symbol),
    };
    gum_process_enumerate_modules(peak_target_collect_module, &context);
    g_ptr_array_sort(resolution->candidates, peak_target_candidate_compare);
    if (resolution->candidates->len > 1) {
        unsigned int best_tier = PEAK_TARGET_MATCH_LEGACY;

        for (gsize i = 0; i < resolution->candidates->len; i++) {
            PeakTargetSymbolCandidate* candidate =
                g_ptr_array_index(resolution->candidates, i);
            if (candidate->match_tier < best_tier) {
                best_tier = candidate->match_tier;
            }
        }

        for (gsize i = resolution->candidates->len; i-- > 0;) {
            PeakTargetSymbolCandidate* candidate =
                g_ptr_array_index(resolution->candidates, i);
            if (candidate->match_tier != best_tier) {
                g_ptr_array_remove_index(resolution->candidates, i);
            }
        }
    }
    peak_target_selector_clear(&selector);
    if (resolution->candidates->len == 0) {
        return PEAK_TARGET_RESOLVE_NONE;
    }
    return resolution->candidates->len == 1 ? PEAK_TARGET_RESOLVE_UNIQUE
                                             : PEAK_TARGET_RESOLVE_AMBIGUOUS;
}

void
peak_target_resolver_print(FILE* stream,
                           const char* selector,
                           const PeakTargetResolution* resolution)
{
    gsize count = resolution != NULL && resolution->candidates != NULL
        ? resolution->candidates->len : 0;

    fprintf(stream, "selector=%s candidates=%" G_GSIZE_FORMAT "\n",
            selector != NULL ? selector : "<null>", count);
    for (gsize i = 0; i < count; i++) {
        const PeakTargetSymbolCandidate* candidate =
            g_ptr_array_index(resolution->candidates, i);
        fprintf(stream,
                "address=%p module=%s mangled=%s demangled=%s\n",
                candidate->address,
                candidate->module,
                candidate->mangled,
                candidate->demangled);
    }
}
