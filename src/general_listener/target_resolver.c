#include "internal/target_resolver.h"

#include "utils/cxx_utils.h"
#include "utils/target_selector_syntax.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    gchar* module;
    gchar* symbol;
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

typedef struct {
    PeakTargetCollectContext* contexts;
    size_t count;
    const char* current_module;
    GHashTable* exact;
    GHashTable* full;
    GHashTable* full_by_name;
    GHashTable* legacy_qualified;
    GHashTable* legacy_short;
} PeakTargetBatchCollectContext;

enum {
    PEAK_TARGET_MATCH_EXACT = 0,
    PEAK_TARGET_MATCH_FULL = 1,
    PEAK_TARGET_MATCH_LEGACY = 2,
};

#if defined(PEAK_TARGET_RESOLVER_TESTING) && defined(PEAK_ENABLE_TEST_HOOKS)
static PeakTargetResolverDiagnostics peak_target_resolver_diagnostics;
#define PEAK_RESOLVER_DIAG_ADD(field, value) \
    (peak_target_resolver_diagnostics.field += (value))
void
peak_target_resolver_reset_diagnostics(void)
{
    memset(&peak_target_resolver_diagnostics, 0,
           sizeof(peak_target_resolver_diagnostics));
}
void
peak_target_resolver_get_diagnostics(PeakTargetResolverDiagnostics* out)
{
    if (out != NULL) {
        *out = peak_target_resolver_diagnostics;
    }
}
#else
#define PEAK_RESOLVER_DIAG_ADD(field, value) ((void)0)
#endif

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

    /* Keep selector validation and PEAK_TARGET splitting on the same lexer. */
    if (!peak_target_selector_next_delimiter(text, '!', FALSE, &separator)) {
        return FALSE;
    }
    if (separator == NULL) {
        *separator_out = NULL;
        return TRUE;
    }
    /* A second top-level '!' is invalid.  The shared lexer also handles
     * operator! and nested punctuation here. */
    {
        const char* extra = NULL;
        if (!peak_target_selector_next_delimiter(separator + 1, '!', FALSE,
                                                  &extra) || extra != NULL) {
            return FALSE;
        }
    }
    *separator_out = separator;
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
        peak_target_selector_has_top_level_offset(selector->symbol)) {
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

gchar*
peak_target_resolver_format_display_name(const char* selector_text,
                                         const char* demangled)
{
    PeakTargetSelector selector;
    gchar* display_name;

    if (demangled == NULL ||
        !peak_target_parse_selector(selector_text, &selector)) {
        return NULL;
    }
    display_name = selector.module != NULL
        ? g_strdup_printf("%s!%s", selector.module, demangled)
        : g_strdup(demangled);
    peak_target_selector_clear(&selector);
    return display_name;
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

    /* `operator()` is the function-id of call-operator overloads.  Its own
     * empty pair is not the overload's parameter list. */
    if (g_str_has_suffix(selector, "operator()")) {
        return FALSE;
    }

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

static void
peak_target_collect_candidate(const GumSymbolDetails* details,
                              PeakTargetCollectContext* context,
                              const char* mangled,
                              const char* demangled,
                              int tier,
                              const char* current_module)
{
    PeakTargetSymbolCandidate* candidate;

    if (!peak_target_resolver_module_matches(context->selector->module,
                                             current_module) ||
        !peak_target_resolver_module_matches(context->module_path,
                                             current_module)) {
        return;
    }

    candidate = g_new0(PeakTargetSymbolCandidate, 1);
    candidate->address = (gpointer)details->address;
    candidate->symbol_address = (gpointer)details->address;
    candidate->size = details->size > 0 ? (gsize)details->size : 0;
    candidate->module = g_strdup(current_module);
    candidate->mangled = g_strdup(mangled);
    candidate->demangled = g_strdup(demangled != NULL ? demangled : mangled);
    candidate->match_tier = (unsigned int)tier;
    for (gsize i = 0; i < context->candidates->len; i++) {
        PeakTargetSymbolCandidate* existing =
            g_ptr_array_index(context->candidates, i);
        if (existing->address == candidate->address &&
            g_strcmp0(existing->module, candidate->module) == 0 &&
            g_strcmp0(existing->demangled, candidate->demangled) == 0) {
            peak_target_symbol_candidate_free(candidate);
            return;
        }
    }
    g_ptr_array_add(context->candidates, candidate);
}

static gboolean
peak_target_string_equal(gconstpointer left, gconstpointer right)
{
    return strcmp(left, right) == 0;
}

static char*
peak_target_full_function_id(const char* signature)
{
    const char* operator_word = strstr(signature, "operator");
    char* name = NULL;

    if (operator_word != NULL) {
        const char* end = strchr(operator_word, '(');
        size_t length = end != NULL ? (size_t)(end - operator_word)
                                    : strlen(operator_word);
        char* operator_name = strndup(operator_word, length);
        char* write = operator_name;

        if (operator_name == NULL) {
            return NULL;
        }
        for (char* read = operator_name; *read != '\0'; read++) {
            if (!g_ascii_isspace(*read)) {
                *write++ = *read;
            }
        }
        *write = '\0';
        free(name);
        return operator_name;
    }
    for (const char* cursor = signature; *cursor != '\0'; ) {
        const char* begin;
        const char* end;

        if (!g_ascii_isalpha(*cursor) && *cursor != '_') {
            cursor++;
            continue;
        }
        begin = cursor++;
        while (g_ascii_isalnum(*cursor) || *cursor == '_') {
            cursor++;
        }
        end = cursor;
        if (*cursor == '<' || *cursor == '(') {
            free(name);
            name = strndup(begin, (size_t)(end - begin));
        }
    }
    return name != NULL ? name : extract_function_name(signature);
}

static void
peak_target_batch_add(GHashTable* table,
                      gchar* key,
                      PeakTargetCollectContext* context)
{
    GPtrArray* matches;

    if (key == NULL) {
        return;
    }
    matches = g_hash_table_lookup(table, key);
    if (matches == NULL) {
        matches = g_ptr_array_new();
        g_hash_table_insert(table, key, matches);
    } else {
        g_free(key);
    }
    g_ptr_array_add(matches, context);
}

static void
peak_target_batch_add_signature_keys(GHashTable* table,
                                     const char* signature,
                                     PeakTargetCollectContext* context)
{
    for (const char* cursor = signature; *cursor != '\0'; ) {
        const char* begin;
        const char* end;

        if (!g_ascii_isalpha(*cursor) && *cursor != '_') {
            cursor++;
            continue;
        }
        begin = cursor++;
        while (g_ascii_isalnum(*cursor) || *cursor == '_') {
            cursor++;
        }
        end = cursor;
        if (*cursor == '<' || *cursor == '(') {
            peak_target_batch_add(table, g_strndup(begin, end - begin), context);
        }
    }
}

static void
peak_target_collect_batch_matches(const GumSymbolDetails* details,
                                  GPtrArray* matches,
                                  const char* mangled,
                                  const char* demangled,
                                  int tier,
                                  const char* current_module)
{
    if (matches == NULL) {
        return;
    }
    PEAK_RESOLVER_DIAG_ADD(candidate_match_evaluations, matches->len);
    for (guint i = 0; i < matches->len; i++) {
        PeakTargetCollectContext* context = g_ptr_array_index(matches, i);
        if (current_module != NULL) {
            peak_target_collect_candidate(details, context, mangled,
                                          demangled, tier, current_module);
        }
    }
}

static void
peak_target_collect_full_matches(const GumSymbolDetails* details,
                                 GPtrArray* matches,
                                 const char* mangled,
                                 const char* demangled,
                                 const char* current_module)
{
    if (matches == NULL) {
        return;
    }
    PEAK_RESOLVER_DIAG_ADD(candidate_match_evaluations, matches->len);
    for (guint i = 0; i < matches->len; i++) {
        PeakTargetCollectContext* context = g_ptr_array_index(matches, i);
        if (current_module != NULL &&
            peak_target_resolver_full_signature_matches(
                context->selector->symbol, demangled)) {
            peak_target_collect_candidate(details, context, mangled, demangled,
                                          PEAK_TARGET_MATCH_FULL, current_module);
        }
    }
}

static void
peak_target_collect_legacy_matches(const GumSymbolDetails* details,
                                   GPtrArray* matches,
                                   const char* mangled,
                                   const char* demangled,
                                   const char* current_module)
{
    if (matches == NULL || peak_target_is_artificial_prefix(demangled)) {
        return;
    }
    peak_target_collect_batch_matches(details, matches, mangled, demangled,
                                      PEAK_TARGET_MATCH_LEGACY, current_module);
}

static gboolean
peak_target_exact_matches_current_module(GPtrArray* matches,
                                         const char* current_module)
{
    if (matches == NULL || current_module == NULL) {
        return FALSE;
    }
    for (guint i = 0; i < matches->len; i++) {
        PeakTargetCollectContext* context = g_ptr_array_index(matches, i);

        if (peak_target_resolver_module_matches(context->selector->module,
                                                current_module) &&
            peak_target_resolver_module_matches(context->module_path,
                                                current_module)) {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
peak_target_collect_batch_symbol(const GumSymbolDetails* details,
                                 gpointer user_data)
{
    PeakTargetBatchCollectContext* batch = user_data;
    GPtrArray* exact_matches;
    char* demangled = NULL;
    gchar* normalized = NULL;
    gchar* qualified = NULL;
    char* short_name = NULL;
    char* legacy_short_name = NULL;

    if (details->type != GUM_SYMBOL_FUNCTION || details->address == 0 ||
        details->name == NULL) {
        return TRUE;
    }
    PEAK_RESOLVER_DIAG_ADD(symbol_visits, 1);
    exact_matches = g_hash_table_lookup(batch->exact, details->name);
    demangled = g_str_has_prefix(details->name, "_Z") &&
                (g_hash_table_size(batch->full) != 0 ||
                 g_hash_table_size(batch->full_by_name) != 0 ||
                 g_hash_table_size(batch->legacy_qualified) != 0 ||
                 g_hash_table_size(batch->legacy_short) != 0)
        ? cxa_demangle(details->name) : NULL;
    /* Exact mangled lookup keeps non-candidates at zero demangles, but the
     * selected symbol must have the same human-readable name whether or not
     * another selector in the batch happens to require global demangling. */
    if (demangled == NULL && g_str_has_prefix(details->name, "_Z") &&
        peak_target_exact_matches_current_module(exact_matches,
                                                 batch->current_module)) {
        demangled = cxa_demangle(details->name);
    }
    if (demangled != NULL) {
        PEAK_RESOLVER_DIAG_ADD(demangles, 1);
    }
    peak_target_collect_batch_matches(details,
                                      exact_matches,
                                      details->name, demangled,
                                      PEAK_TARGET_MATCH_EXACT, batch->current_module);
    if (demangled != NULL) {
        normalized = peak_target_without_space(demangled);
        peak_target_collect_batch_matches(details,
                                          g_hash_table_lookup(batch->full,
                                                              normalized),
                                          details->name, demangled,
                                          PEAK_TARGET_MATCH_FULL, batch->current_module);
        short_name = peak_target_full_function_id(demangled);
        peak_target_collect_full_matches(details,
                                         g_hash_table_lookup(batch->full_by_name,
                                                             short_name),
                                         details->name, demangled, batch->current_module);
        for (const char* cursor = demangled; *cursor != '\0'; ) {
            const char* begin;
            const char* end;
            gchar* key;
            if (!g_ascii_isalpha(*cursor) && *cursor != '_') { cursor++; continue; }
            begin = cursor++;
            while (g_ascii_isalnum(*cursor) || *cursor == '_') cursor++;
            end = cursor;
            if (*cursor != '<' && *cursor != '(') continue;
            key = g_strndup(begin, end - begin);
            peak_target_collect_full_matches(details,
                g_hash_table_lookup(batch->full_by_name, key),
                details->name, demangled, batch->current_module);
            g_free(key);
        }
        qualified = peak_target_without_final_parameters(demangled);
        {
            char* return_prefix = strrchr(qualified, ' ');
            if (return_prefix != NULL) {
                memmove(qualified, return_prefix + 1,
                        strlen(return_prefix + 1) + 1);
            }
        }
        {
            gchar* normalized_qualified = peak_target_without_space(qualified);
            peak_target_collect_legacy_matches(details,
                                               g_hash_table_lookup(
                                                   batch->legacy_qualified,
                                                   normalized_qualified),
                                               details->name, demangled, batch->current_module);
            g_free(normalized_qualified);
        }
        if (g_hash_table_size(batch->legacy_short) != 0) {
            legacy_short_name = extract_function_name(demangled);
            peak_target_collect_legacy_matches(
                details,
                g_hash_table_lookup(batch->legacy_short, legacy_short_name),
                details->name, demangled, batch->current_module);
        }
    }
    free(legacy_short_name);
    free(short_name);
    g_free(qualified);
    g_free(normalized);
    free(demangled);
    return TRUE;
}

static gboolean
peak_target_batch_module_is_applicable(PeakTargetBatchCollectContext* batch,
                                       const char* module_path)
{
    for (size_t i = 0; i < batch->count; i++) {
        PeakTargetCollectContext* context = &batch->contexts[i];

        if (context->selector != NULL &&
            peak_target_resolver_module_matches(context->selector->module,
                                                module_path) &&
            peak_target_resolver_module_matches(context->module_path,
                                                module_path)) {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
peak_target_collect_batch_module(GumModule* module, gpointer user_data)
{
    PeakTargetBatchCollectContext* batch = user_data;
    const char* path = gum_module_get_path(module);

    if (!peak_target_batch_module_is_applicable(batch, path)) {
        return TRUE;
    }
    batch->current_module = path != NULL ? path : "<unknown>";
    PEAK_RESOLVER_DIAG_ADD(module_symbol_enumerations, 1);
    gum_module_enumerate_symbols(module, peak_target_collect_batch_symbol,
                                 batch);
    batch->current_module = NULL;
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

static PeakTargetResolveResult
peak_target_resolution_finalize(PeakTargetResolution* resolution)
{
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
    if (resolution->candidates->len == 0) {
        return PEAK_TARGET_RESOLVE_NONE;
    }
    return resolution->candidates->len == 1 ? PEAK_TARGET_RESOLVE_UNIQUE
                                             : PEAK_TARGET_RESOLVE_AMBIGUOUS;
}

void
peak_target_resolver_resolve_many(PeakTargetResolveRequest* requests,
                                  size_t count)
{
    PeakTargetSelector* selectors;
    PeakTargetCollectContext* contexts;
    PeakTargetBatchCollectContext batch;
    size_t active = 0;

    if (requests == NULL || count == 0) {
        return;
    }
    selectors = g_try_new0(PeakTargetSelector, count);
    contexts = g_try_new0(PeakTargetCollectContext, count);
    if (selectors == NULL || contexts == NULL) {
        g_free(contexts);
        g_free(selectors);
        for (size_t i = 0; i < count; i++) {
            peak_target_resolution_clear(&requests[i].resolution);
            requests[i].result = PEAK_TARGET_RESOLVE_NONE;
        }
        return;
    }
    batch.exact = g_hash_table_new_full(g_str_hash, peak_target_string_equal, g_free,
                                        (GDestroyNotify)g_ptr_array_unref);
    batch.full = g_hash_table_new_full(g_str_hash, peak_target_string_equal, g_free,
                                       (GDestroyNotify)g_ptr_array_unref);
    batch.full_by_name = g_hash_table_new_full(
        g_str_hash, peak_target_string_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    batch.legacy_qualified = g_hash_table_new_full(
        g_str_hash, peak_target_string_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    batch.legacy_short = g_hash_table_new_full(g_str_hash, peak_target_string_equal, g_free,
                                               (GDestroyNotify)g_ptr_array_unref);
    for (size_t i = 0; i < count; i++) {
        peak_target_resolution_clear(&requests[i].resolution);
        requests[i].result = PEAK_TARGET_RESOLVE_INVALID;
        if (!peak_target_parse_selector(requests[i].selector, &selectors[i])) {
            continue;
        }
        requests[i].resolution.candidates = g_ptr_array_new_with_free_func(
            peak_target_symbol_candidate_free);
        gboolean mangled_selector = g_str_has_prefix(selectors[i].symbol, "_Z");
        gboolean human_signature = !mangled_selector &&
            (strchr(selectors[i].symbol, '(') != NULL ||
             strchr(selectors[i].symbol, '<') != NULL);
        gboolean complete_signature = human_signature &&
            peak_target_selector_is_complete_signature(selectors[i].symbol);

        contexts[i] = (PeakTargetCollectContext){
            .selector = &selectors[i],
            .module_path = requests[i].module_path,
            .candidates = requests[i].resolution.candidates,
            .cxx_selector = human_signature || requests[i].allow_legacy_short,
        };
        active++;
        peak_target_batch_add(batch.exact, g_strdup(selectors[i].symbol),
                              &contexts[i]);
        if (human_signature) {
            peak_target_batch_add(batch.full,
                                  peak_target_without_space(selectors[i].symbol),
                                  &contexts[i]);
        }
        if (complete_signature) {
            {
                char* function_name;

                function_name = peak_target_full_function_id(selectors[i].symbol);
                if (function_name != NULL) {
                    peak_target_batch_add(batch.full_by_name,
                                          g_strdup(function_name), &contexts[i]);
                    free(function_name);
                }
                peak_target_batch_add_signature_keys(batch.full_by_name,
                                                     selectors[i].symbol,
                                                     &contexts[i]);
            }
        } else if (!mangled_selector && requests[i].allow_legacy_short) {
            if (strstr(selectors[i].symbol, "::") != NULL) {
                peak_target_batch_add(batch.legacy_qualified,
                                      peak_target_without_space(selectors[i].symbol),
                                      &contexts[i]);
            } else {
                peak_target_batch_add(batch.legacy_short,
                                      g_strdup(selectors[i].symbol),
                                      &contexts[i]);
            }
        }
    }
    batch.contexts = contexts;
    batch.count = count;
    if (active == 0) {
        g_hash_table_unref(batch.legacy_short);
        g_hash_table_unref(batch.legacy_qualified);
        g_hash_table_unref(batch.full_by_name);
        g_hash_table_unref(batch.full);
        g_hash_table_unref(batch.exact);
        g_free(contexts);
        g_free(selectors);
        return;
    }
    PEAK_RESOLVER_DIAG_ADD(module_passes, 1);
    gum_process_enumerate_modules(peak_target_collect_batch_module, &batch);
    for (size_t i = 0; i < count; i++) {
        if (contexts[i].selector != NULL) {
            requests[i].result = peak_target_resolution_finalize(
                &requests[i].resolution);
            peak_target_selector_clear(&selectors[i]);
        }
    }
    g_hash_table_unref(batch.legacy_short);
    g_hash_table_unref(batch.legacy_qualified);
    g_hash_table_unref(batch.full_by_name);
    g_hash_table_unref(batch.full);
    g_hash_table_unref(batch.exact);
    g_free(contexts);
    g_free(selectors);
}

PeakTargetResolveResult
peak_target_resolver_resolve(const char* selector_text,
                             const char* module_path,
                             gboolean allow_legacy_short,
                             PeakTargetResolution* resolution)
{
    PeakTargetResolveRequest request = {
        .selector = selector_text,
        .module_path = module_path,
        .allow_legacy_short = allow_legacy_short,
    };

    if (resolution == NULL) {
        return PEAK_TARGET_RESOLVE_INVALID;
    }
    request.resolution = *resolution;
    peak_target_resolver_resolve_many(&request, 1);
    *resolution = request.resolution;
    return request.result;
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
