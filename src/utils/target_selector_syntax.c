#include "utils/target_selector_syntax.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool grouping;
    char closer;
    size_t id;
} PeakSelectorFrame;

typedef struct {
    size_t frame;
} PeakSelectorAngle;

static bool
peak_selector_operator_token_ends_at(const char* text, const char* cursor)
{
    while (cursor > text && isspace((unsigned char)cursor[-1])) {
        cursor--;
    }
    return cursor - text >= 8 && memcmp(cursor - 8, "operator", 8) == 0 &&
           (cursor - 8 == text ||
            (!isalnum((unsigned char)cursor[-9]) && cursor[-9] != '_'));
}

static bool
peak_selector_operator_punctuation_at(const char* text, const char* cursor)
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
            (word > text && (isalnum((unsigned char)word[-1]) || word[-1] == '_'))) {
            continue;
        }
        token = word + 8;
        while (isspace((unsigned char)*token)) {
            token++;
        }
        for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); i++) {
            size_t length = strlen(tokens[i]);
            if (strncmp(token, tokens[i], length) == 0 &&
                cursor >= token && cursor < token + length) {
                return true;
            }
        }
    }
    return false;
}

static bool
peak_selector_preceding_keyword(const char* text,
                                const char* cursor,
                                const char* keyword)
{
    const char* end = cursor;
    size_t length = strlen(keyword);

    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    if ((size_t)(end - text) < length || memcmp(end - length, keyword, length) != 0) {
        return false;
    }
    return end - length == text ||
           (!isalnum((unsigned char)(end - length)[-1]) &&
            (end - length)[-1] != '_');
}

static bool
peak_selector_parenthesis_is_grouping(const char* text, const char* cursor)
{
    while (cursor > text && isspace((unsigned char)cursor[-1])) {
        cursor--;
    }
    if (cursor == text ||
        peak_selector_preceding_keyword(text, cursor, "decltype") ||
        peak_selector_preceding_keyword(text, cursor, "noexcept") ||
        peak_selector_preceding_keyword(text, cursor, "sizeof") ||
        peak_selector_preceding_keyword(text, cursor, "alignof") ||
        peak_selector_preceding_keyword(text, cursor, "typeid")) {
        return true;
    }
    return strchr("<[{,(+-*/%&|^~!=?>", cursor[-1]) != NULL;
}

static bool
peak_selector_frame_has_angle(const PeakSelectorAngle* angles,
                              size_t angle_count,
                              size_t frame)
{
    return angle_count > 0 && angles[angle_count - 1].frame == frame;
}

static bool
peak_selector_grow(void** values, size_t* capacity, size_t value_size)
{
    size_t new_capacity;
    void* resized;

    if (*capacity > SIZE_MAX / 2 ||
        *capacity * 2 > SIZE_MAX / value_size) {
        return false;
    }
    new_capacity = *capacity * 2;
    resized = realloc(*values, new_capacity * value_size);
    if (resized == NULL) {
        return false;
    }
    *values = resized;
    *capacity = new_capacity;
    return true;
}

bool
peak_target_selector_next_delimiter(const char* text,
                                    char delimiter,
                                    bool recover_nested,
                                    const char** delimiter_out)
{
    PeakSelectorFrame* frames;
    PeakSelectorAngle* angles;
    size_t frame_count = 1;
    size_t angle_count = 0;
    size_t frame_capacity = 8;
    size_t angle_capacity = 8;
    size_t next_frame = 1;
    const char* nested = NULL;

    if (text == NULL || delimiter == '\0' || delimiter_out == NULL) {
        return false;
    }
    *delimiter_out = NULL;
    frames = calloc(frame_capacity, sizeof(*frames));
    angles = calloc(angle_capacity, sizeof(*angles));
    if (frames == NULL || angles == NULL) {
        free(angles);
        free(frames);
        return false;
    }
    for (const char* cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '(' || *cursor == '[' || *cursor == '{') {
            if (frame_count == frame_capacity &&
                !peak_selector_grow((void**)&frames, &frame_capacity,
                                    sizeof(*frames))) {
                goto invalid;
            }
            frames[frame_count++] = (PeakSelectorFrame){
                .grouping = *cursor == '(' ?
                    peak_selector_parenthesis_is_grouping(text, cursor) : true,
                .closer = *cursor == '(' ? ')' : (*cursor == '[' ? ']' : '}'),
                .id = next_frame++,
            };
        } else if (*cursor == ')' || *cursor == ']' || *cursor == '}') {
            PeakSelectorFrame current;
            if (frame_count == 1) {
                goto invalid;
            }
            current = frames[frame_count - 1];
            if (*cursor != current.closer) {
                goto invalid;
            }
            if (peak_selector_frame_has_angle(angles, angle_count, current.id)) {
                if (!current.grouping) {
                    goto invalid;
                }
                while (peak_selector_frame_has_angle(angles, angle_count, current.id)) {
                    angle_count--;
                }
            }
            frame_count--;
        } else if (*cursor == '<' && !peak_selector_operator_punctuation_at(text, cursor) &&
                   cursor[1] != '=') {
            if (angle_count == angle_capacity &&
                !peak_selector_grow((void**)&angles, &angle_capacity,
                                    sizeof(*angles))) {
                goto invalid;
            }
            angles[angle_count++].frame = frames[frame_count - 1].id;
        } else if (*cursor == '>' && !peak_selector_operator_punctuation_at(text, cursor)) {
            if (cursor > text && cursor[-1] == '=') {
                goto invalid;
            }
            if (cursor[1] != '=' && peak_selector_frame_has_angle(
                    angles, angle_count, frames[frame_count - 1].id)) {
                angle_count--;
            } else if (cursor[1] != '=' && !frames[frame_count - 1].grouping) {
                goto invalid;
            }
        } else if (*cursor == delimiter &&
                   !peak_selector_operator_punctuation_at(text, cursor) &&
                   !(delimiter == ',' && peak_selector_operator_token_ends_at(text, cursor))) {
            if (frame_count == 1 && angle_count == 0) {
                *delimiter_out = cursor;
                free(angles);
                free(frames);
                return true;
            }
            if (nested == NULL) {
                nested = cursor;
            }
        }
    }
    if (frame_count == 1 && angle_count == 0) {
        free(angles);
        free(frames);
        return true;
    }
invalid:
    if (recover_nested && nested != NULL) {
        *delimiter_out = nested;
    }
    free(angles);
    free(frames);
    return recover_nested;
}

bool
peak_target_selector_has_top_level_offset(const char* text)
{
    const char* cursor = text;

    while (cursor != NULL && *cursor != '\0') {
        const char* plus = NULL;

        if (!peak_target_selector_next_delimiter(cursor, '+', false, &plus)) {
            return false;
        }
        if (plus == NULL) {
            return false;
        }
        if (plus[1] == '0' && (plus[2] == 'x' || plus[2] == 'X')) {
            return true;
        }
        cursor = plus + 1;
    }
    return false;
}
