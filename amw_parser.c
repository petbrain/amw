#include <errno.h>
#include <limits.h>
#include <stdio.h>

#include <amw.h>

#define DEFAULT_LINE_CAPACITY  250

#ifdef TRACE_ENABLED
    static unsigned tracelevel = 0;

#   define _TRACE_INDENT() \
        for (unsigned i = 0; i < tracelevel * 4; i++) {  \
            fputc(' ', stderr);  \
        }

#   define _TRACE_POS()  \
        _TRACE_INDENT() \
        fprintf(stderr, "%s; line %u, block indent %u", \
                __func__, parser->line_number, parser->block_indent);

#   define TRACE_ENTER() \
        do {  \
            _TRACE_POS() \
            fputs(" {\n", stderr);  \
            tracelevel++; \
        } while (false)

#   define TRACE_EXIT() \
        do {  \
            tracelevel--; \
            _TRACE_INDENT() \
            fputs("}\n", stderr);  \
        } while (false)

#   define TRACEPOINT()  \
        do {  \
            _TRACE_POS() \
            fputc('\n', stderr);  \
        } while (false)

#   define TRACE(...)  \
        do {  \
            _TRACE_INDENT() \
            fprintf(stderr, "%s: ", __func__); \
            fprintf(stderr, __VA_ARGS__);  \
            fputc('\n', stderr);  \
        } while (false)
#else
#   define TRACEPOINT()
#   define TRACE_ENTER()
#   define TRACE_EXIT()
#   define TRACE(...)
#endif

// forward declarations
static UwResult parse_value(AmwParser* parser, unsigned* nested_value_pos);
static UwResult value_parser_func(AmwParser* parser);
static UwResult parse_raw_value(AmwParser* parser);
static UwResult parse_literal_string(AmwParser* parser);
static UwResult parse_folded_string(AmwParser* parser);
static UwResult parse_isodate(AmwParser* parser);
static UwResult parse_timestamp(AmwParser* parser);


AmwParser* amw_create_parser(UwValuePtr markup)
{
    AmwParser* parser = allocate(sizeof(AmwParser), true);
    if (!parser) {
        return nullptr;
    }
    parser->markup = uw_clone(markup);

    parser->blocklevel = 1;
    parser->max_blocklevel = AMW_MAX_RESURSION_DEPTH;

    parser->skip_comments = true;

    UwValue status = UwNull();

    parser->current_line = uw_create_empty_string(DEFAULT_LINE_CAPACITY, 1);
    if (uw_error(&parser->current_line)) {
        goto error;
    }
    parser->custom_parsers = UwMap(
        UwCharPtr("raw"),       UwPtr((void*) parse_raw_value),
        UwCharPtr("literal"),   UwPtr((void*) parse_literal_string),
        UwCharPtr("folded"),    UwPtr((void*) parse_folded_string),
        UwCharPtr("isodate"),   UwPtr((void*) parse_isodate),
        UwCharPtr("timestamp"), UwPtr((void*) parse_timestamp),
        UwCharPtr("json"),      UwPtr((void*) _amw_json_parser_func)
    );
    if (uw_error(&parser->custom_parsers)) {
        goto error;
    }

    status = uw_start_read_lines(markup);
    if (uw_error(&status)) {
        goto error;
    }

    return parser;

error:
    amw_delete_parser(&parser);
    return nullptr;
}

void amw_delete_parser(AmwParser** parser_ptr)
{
    AmwParser* parser = *parser_ptr;
    *parser_ptr = nullptr;
    uw_destroy(&parser->markup);
    uw_destroy(&parser->current_line);
    uw_destroy(&parser->custom_parsers);
    release((void**) &parser, sizeof(AmwParser));
}

bool amw_set_custom_parser(AmwParser* parser, char* convspec, AmwBlockParserFunc parser_func)
{
    UWDECL_CharPtr(key, convspec);
    UWDECL_Ptr(value, (void*) parser_func);
    return uw_map_update(&parser->custom_parsers, &key, &value);
}

static inline bool have_custom_parser(AmwParser* parser, UwValuePtr convspec)
{
    return uw_map_has_key(&parser->custom_parsers, convspec);
}

static inline AmwBlockParserFunc get_custom_parser(AmwParser* parser, UwValuePtr convspec)
{
    UwValue parser_func = uw_map_get(&parser->custom_parsers, convspec);
    uw_assert_ptr(&parser_func);
    return (AmwBlockParserFunc) (parser_func.ptr);
}

UwResult _amw_parser_error(AmwParser* parser, unsigned line_number, unsigned pos, char* description, ...)
{
    UwValue status = uw_create(UwTypeId_AmwStatus);
    // status is UW_SUCCESS by default
    if (uw_error(&status)) {
        return uw_move(&status);
    }
    status.status_code = AMW_PARSE_ERROR;
    AmwStatusData* status_data = _amw_status_data_ptr(&status);
    status_data->line_number = line_number;;
    status_data->position = pos;
    va_list ap;
    va_start(ap);
    _uw_set_status_desc_ap(&status, description, ap);
    va_end(ap);
    return uw_move(&status);
}

static inline UwResult parser_error(AmwParser* parser, unsigned pos, char* description)
{
    return _amw_parser_error(parser, parser->line_number, pos, description);
}

bool _amw_end_of_block(UwValuePtr status)
{
    return (status->type_id == UwTypeId_Status) && (status->status_code == AMW_END_OF_BLOCK);
}

static inline bool end_of_line(UwValuePtr str, unsigned position)
/*
 * Return true if position is beyond end of line.
 */
{
    return !uw_string_index_valid(str, position);
}

static inline bool isspace_or_eol_at(UwValuePtr str, unsigned position)
{
    if (end_of_line(str, position)) {
        return true;
    } else {
        return uw_isspace(uw_char_at(str, position));
    }
}

static UwResult read_line(AmwParser* parser)
/*
 * Read line into parser->current line and strip trailing spaces.
 * Return status.
 */
{
    UwValue status = uw_read_line_inplace(&parser->markup, &parser->current_line);
    if (uw_error(&status)) {
        return uw_move(&status);
    }

    // strip trailing spaces
    if (!uw_string_rtrim(&parser->current_line)) {
        return UwOOM();
    }

    // measure indent
    parser->current_indent = uw_string_skip_spaces(&parser->current_line, 0);

    // set current_line
    parser->line_number = uw_get_line_number(&parser->markup);

    return UwOK();
}

static inline bool is_comment_line(AmwParser* parser)
/*
 * Return true if current line starts with AMW_COMMENT char.
 */
{
    return uw_char_at(&parser->current_line, parser->current_indent) == AMW_COMMENT;
}

UwResult _amw_read_block_line(AmwParser* parser)
{
    TRACEPOINT();

    if (parser->eof) {
        if (parser->blocklevel) {
            // continue returning this for nested blocks
            return UwError(AMW_END_OF_BLOCK);
        }
        return UwError(UW_ERROR_EOF);
    }
    for (;;) {{
        UwValue status = read_line(parser);
        if (uw_eof(&status)) {
            parser->eof = true;
            uw_destroy(&parser->current_line);
            return UwError(AMW_END_OF_BLOCK);
        }
        if (uw_error(&status)) {
            return uw_move(&status);
        }
        if (parser->skip_comments) {
            // skip empty lines too
            if (uw_strlen(&parser->current_line) == 0) {
                continue;
            }
            if (is_comment_line(parser)) {
                continue;
            }
            parser->skip_comments = false;
        }
        if (uw_strlen(&parser->current_line) == 0) {
            // return empty line as is
            return UwOK();
        }
        if (parser->current_indent >= parser->block_indent) {
            // indentation is okay, return line
            return UwOK();
        }
        // unindent detected
        if (is_comment_line(parser)) {
            // skip unindented comments
            continue;
        }
        TRACE("unindent");
        // end of block
        if (!uw_unread_line(&parser->markup, &parser->current_line)) {
            return UwError(UW_ERROR_UNREAD_FAILED);
        }
        uw_string_truncate(&parser->current_line, 0);
        return UwError(AMW_END_OF_BLOCK);
    }}
}

UwResult _amw_read_block(AmwParser* parser)
{
    TRACEPOINT();

    UwValue lines = UwList();
    if (uw_error(&lines)) {
        return uw_move(&lines);
    }
    for (;;) {{
        // append line
        UwValue line = uw_substr(&parser->current_line, parser->block_indent, UINT_MAX);
        if (uw_error(&line)) {
            return uw_move(&line);
        }
        if (!uw_list_append(&lines, &line)) {
            return UwOOM();
        }
        // read next line
        UwValue status = _amw_read_block_line(parser);
        if (_amw_end_of_block(&status)) {
            return uw_move(&lines);
        }
        if (uw_error(&status)) {
            return uw_move(&status);
        }
    }}
}

static UwResult parse_nested_block(AmwParser* parser, unsigned block_pos, AmwBlockParserFunc parser_func)
/*
 * Set block indent to `block_pos` and call parser_func.
 */
{
    if (parser->blocklevel >= parser->max_blocklevel) {
        return parser_error(parser, parser->current_indent, "Too many nested blocks");
    }

    // start nested block
    parser->blocklevel++;
    unsigned saved_block_indent = parser->block_indent;
    parser->block_indent = block_pos;

    TRACE_ENTER();

    // call parser function
    UwValue result = parser_func(parser);

    // end nested block
    parser->block_indent = saved_block_indent;
    parser->blocklevel--;

    TRACE_EXIT();
    return uw_move(&result);
}

static UwResult parse_nested_block_from_next_line(AmwParser* parser, AmwBlockParserFunc parser_func)
/*
 * Read next line, set block indent to current indent plus one, and call parser_func.
 */
{
    TRACEPOINT();
    TRACE("new block_pos %u", parser->block_indent + 1);

    // temporarily increment block indent by one and read next line
    parser->block_indent++;
    UwValue status = _amw_read_block_line(parser);
    parser->block_indent--;

    if (_amw_end_of_block(&status)) {
        return parser_error(parser, parser->current_indent, "Empty block");
    }
    if (uw_error(&status)) {
        return uw_move(&status);
    }

    // call parse_nested_block
    return parse_nested_block(parser, parser->block_indent + 1, parser_func);
}

static unsigned get_start_position(AmwParser* parser)
/*
 * Return position of the first non-space character in the current block.
 * The block may start inside `current_line` for nested values of list or map.
 */
{
    if (parser->block_indent < parser->current_indent) {
        return parser->current_indent;
    } else {
        return uw_string_skip_spaces(&parser->current_line, parser->block_indent);
    }
}

static UwResult parse_convspec(AmwParser* parser, unsigned opening_colon_pos, unsigned* end_pos)
/*
 * Extract conversion specifier starting from `opening_colon_pos` in the `current_line`.
 *
 * On success return string and write position of the value closing colon to `closing_colon_pos`.
 *
 * If conversion specified is not detected, return UwNull()
 *
 * On error return UwStatus.
 */
{
    unsigned start_pos = opening_colon_pos + 1;

    if (!uw_strchr(&parser->current_line, ':', start_pos, end_pos)) {
        return UwNull();
    }
    if (*end_pos == start_pos) {
        // empty conversion specifier
        return UwNull();
    }
    if (!isspace_or_eol_at(&parser->current_line, *end_pos + 1)) {
        // not a conversion specifier
        return UwNull();
    }
    UwValue convspec = uw_substr(&parser->current_line, start_pos, *end_pos);
    if (uw_error(&convspec)) {
        return uw_move(&convspec);
    }
    if (!uw_string_trim(&convspec)) {
        return UwOOM();
    }
    if (!have_custom_parser(parser, &convspec)) {
        // such a conversion specifier is not defined
        return UwNull();
    }
    return uw_move(&convspec);
}

static UwResult parse_raw_value(AmwParser* parser)
{
    TRACEPOINT();

    UwValue lines = _amw_read_block(parser);
    if (uw_error(&lines)) {
        return uw_move(&lines);
    }
    if (uw_list_length(&lines) > 1) {
        // append one empty line for ending line break
        UWDECL_String(empty_line);
        if (!uw_list_append(&lines, &empty_line)) {
            return UwOOM();
        }
    }
    // return concatenated lines
    return uw_list_join('\n', &lines);
}

static UwResult parse_literal_string(AmwParser* parser)
/*
 * Parse current block as a literal string.
 */
{
    TRACEPOINT();

    UwValue lines = _amw_read_block(parser);
    if (uw_error(&lines)) {
        return uw_move(&lines);
    }

    // normalize list of lines

    if (!uw_list_dedent(&lines)) {
        return UwOOM();
    }
    // drop empty trailing lines
    unsigned len = uw_list_length(&lines);
    while (len--) {{
        UwValue line = uw_list_item(&lines, len);
        if (uw_strlen(&line) != 0) {
            break;
        }
        uw_list_del(&lines, len, len + 1);
    }}

    // append one empty line for ending line break
    if (uw_list_length(&lines) > 1) {
        UWDECL_String(empty_line);
        if (!uw_list_append(&lines, &empty_line)) {
            return UwOOM();
        }
    }

    // return concatenated lines
    return uw_list_join('\n', &lines);
}

static UwResult parse_folded_string(AmwParser* parser)
{
    TRACEPOINT();

    UwValue lines = _amw_read_block(parser);
    if (uw_error(&lines)) {
        return uw_move(&lines);
    }

    // normalize list of lines

    if (!uw_list_dedent(&lines)) {
        return UwOOM();
    }
    // drop empty lines
    unsigned len = uw_list_length(&lines);
    for (unsigned i = len; i--;) {{
        UwValue line = uw_list_item(&lines, i);
        if (uw_strlen(&line) == 0) {
            uw_list_del(&lines, i, i + 1);
            len--;
        }
    }}
    if (len == 0) {
        // return empty string
        return UwString();
    }

    // return concatenated lines
    return uw_list_join(' ', &lines);
}

UwResult _amw_unescape_line(AmwParser* parser, UwValuePtr line, unsigned line_number,
                            char32_t quote, unsigned start_pos, unsigned* end_pos)
{
    unsigned len = uw_strlen(line);
    if (start_pos >= len) {
        if (end_pos) {
            *end_pos = start_pos;
        }
        return UwString();
    }
    UwValue result = uw_create_empty_string(
        len - start_pos,  // guesswork
        uw_string_char_size(line)
    );
    unsigned pos = start_pos;
    while (pos < len) {
        char32_t chr = uw_char_at(line, pos);
        if (chr == quote) {
            // closing quote detected
            break;
        }
        if (chr != '\\') {
            if (!uw_string_append(&result, chr)) {
                return UwOOM();
            }
        } else {
            // start of escape sequence
            pos++;
            if (end_of_line(line, pos)) {
                if (!uw_string_append(&result, chr)) {  // leave backslash in the result
                    return UwOOM();
                }
                return UwOK();
            }
            bool append_ok = false;
            int hexlen;
            chr = uw_char_at(line, pos);
            switch (chr) {

                // Simple escape sequences
                case '\'':    //  \'   single quote     byte 0x27
                case '"':     //  \"   double quote     byte 0x22
                case '?':     //  \?   question mark    byte 0x3f
                case '\\':    //  \\   backslash        byte 0x5c
                    append_ok = uw_string_append(&result, chr);
                    break;
                case 'a': append_ok = uw_string_append(&result, 0x07); break;  // audible bell
                case 'b': append_ok = uw_string_append(&result, 0x08); break;  // backspace
                case 'f': append_ok = uw_string_append(&result, 0x0c); break;  // form feed
                case 'n': append_ok = uw_string_append(&result, 0x0a); break;  // line feed
                case 'r': append_ok = uw_string_append(&result, 0x0d); break;  // carriage return
                case 't': append_ok = uw_string_append(&result, 0x09); break;  // horizontal tab
                case 'v': append_ok = uw_string_append(&result, 0x0b); break;  // vertical tab

                // Numeric escape sequences
                case 'o': {
                    //  \on{1:3} code unit n... (1-3 octal digits)
                    char32_t v = 0;
                    for (int i = 0; i < 3; i++) {
                        pos++;
                        if (end_of_line(line, pos)) {
                            if (i == 0) {
                                return _amw_parser_error(parser, line_number, pos, "Incomplete octal value");
                            }
                            break;
                        }
                        char32_t c = uw_char_at(line, pos);
                        if ('0' <= c && c <= '7') {
                            v <<= 3;
                            v += c - '0';
                        } else {
                            return _amw_parser_error(parser, line_number, pos, "Bad octal value");
                        }
                    }
                    append_ok = uw_string_append(&result, v);
                    break;
                }
                case 'x':
                    //  \xn{2}   code unit n... (exactly 2 hexadecimal digits are required)
                    hexlen = 2;
                    goto parse_hex_value;

                // Unicode escape sequences
                case 'u':
                    //  \un{4}  code point U+n... (exactly 4 hexadecimal digits are required)
                    hexlen = 4;
                    goto parse_hex_value;
                case 'U':
                    //  \Un{8}  code point U+n... (exactly 8 hexadecimal digits are required)
                    hexlen = 8;

                parse_hex_value: {
                    char32_t v = 0;
                    for (int i = 0; i < hexlen; i++) {
                        pos++;
                        if (end_of_line(line, pos)) {
                            return _amw_parser_error(parser, line_number, pos, "Incomplete hexadecimal value");
                        }
                        char32_t c = uw_char_at(line, pos);
                        if ('0' <= c && c <= '9') {
                            v <<= 4;
                            v += c - '0';
                        } else if ('a' <= c && c <= 'f') {
                            v <<= 4;
                            v += c - 'a' + 10;
                        } else if ('A' <= c && c <= 'F') {
                            v <<= 4;
                            v += c - 'A' + 10;
                        } else {
                            return _amw_parser_error(parser, line_number, pos, "Bad hexadecimal value");
                        }
                    }
                    append_ok = uw_string_append(&result, v);
                    break;
                }
                default:
                    // not a valid escape sequence
                    append_ok = uw_string_append(&result, '\\');
                    if (append_ok) {
                        append_ok = uw_string_append(&result, chr);
                    }
                    break;
            }
            if (!append_ok) {
                return UwOOM();
            }
        }
        pos++;
    }
    if (end_pos) {
        *end_pos = pos;
    }
    return uw_move(&result);
}

static bool find_closing_quote(UwValuePtr line, char32_t quote, unsigned start_pos, unsigned* end_pos)
/*
 * Helper function for parse_quoted_string.
 * Search for closing quote in escaped line.
 * If found, write its position to `end_pos` and return true;
 */
{
    for (;;) {
        if (!uw_strchr(line, quote, start_pos, end_pos)) {
            return false;
        }
        // check if the quote is not escaped
        if (*end_pos && uw_char_at(line, *end_pos - 1) == '\\') {
            // continue searching
            start_pos = *end_pos + 1;
        } else {
            (*end_pos)++;
            return true;
        }
    }
}

static UwResult parse_quoted_string(AmwParser* parser, unsigned opening_quote_pos, unsigned* end_pos)
/*
 * Parse quoted string starting from `opening_quote_pos` in the current line.
 *
 * Write next position after the closing quote to `end_pos`.
 */
{
    TRACEPOINT();

    // Get opening quote. The closing quote should be the same.
    char32_t quote = uw_char_at(&parser->current_line, opening_quote_pos);

    // process first line
    if (find_closing_quote(&parser->current_line, quote, opening_quote_pos + 1, end_pos)) {
        // single-line string
        return _amw_unescape_line(parser, &parser->current_line, parser->line_number,
                                  quote, opening_quote_pos + 1, nullptr);
    }

    // start nested block for reading multi-line string
    unsigned saved_block_indent = parser->block_indent;
    parser->block_indent = opening_quote_pos + 1;

    // read block
    UwValue lines = UwList();
    if (uw_error(&lines)) {
        return uw_move(&lines);
    }
    UwValue line_numbers = UwList();
    if (uw_error(&line_numbers)) {
        return uw_move(&line_numbers);
    }
    bool closing_quote_detected = false;
    for (;;) {{
        // append line
        UwValue line = uw_substr(&parser->current_line, parser->block_indent, UINT_MAX);
        if (uw_error(&line)) {
            return uw_move(&line);
        }
        // append line number
        UwValue n = UwUnsigned(parser->line_number);
        if (!uw_list_append(&line_numbers, &n)) {
            return UwOOM();
        }
        if (find_closing_quote(&parser->current_line, quote, opening_quote_pos + 1, end_pos)) {
            // final line
            UwValue final_line = uw_substr(&line, 0, *end_pos - 1);
            if (!uw_list_append(&lines, &final_line)) {
                return UwOOM();
            }
            closing_quote_detected = true;
            break;
        }
        if (!uw_list_append(&lines, &line)) {
            return UwOOM();
        }
        // read next line
        UwValue status = _amw_read_block_line(parser);
        if (_amw_end_of_block(&status)) {
            break;
        }
        if (uw_error(&status)) {
            return uw_move(&status);
        }
    }}

    // end nested block
    parser->block_indent = saved_block_indent;

    if (!closing_quote_detected) {
        // check if current_line (i.e. next line after the block)
        // starts with a quote that has the same indent as the opening quote
        if (parser->current_indent == opening_quote_pos
            && uw_char_at(&parser->current_line, parser->current_indent) == quote) {

            *end_pos = opening_quote_pos + 1;
        } else {
            return parser_error(parser, parser->current_indent, "String contains no closing quote");
        }
    }

    // fold lines

    if (!uw_list_dedent(&lines)) {
        return UwOOM();
    }

    // drop empty lines
    unsigned len = uw_list_length(&lines);
    for (unsigned i = len; i--;) {{
        UwValue line = uw_list_item(&lines, i);
        if (uw_strlen(&line) == 0) {
            uw_list_del(&lines, i, i + 1);
            uw_list_del(&line_numbers, i, i + 1);
            len--;
        }
    }}
    if (len == 0) {
        // return empty string
        return UwString();
    }

    // unescape lines
    for (unsigned i = 0; i < len; i++) {{
        UwValue line = uw_list_item(&lines, i);
        if (uw_error(&line)) {
            return uw_move(&line);
        }
        UwValue line_number = uw_list_item(&lines, i);
        if (uw_error(&line_number)) {
            return uw_move(&line_number);
        }
        UwValue unescaped = _amw_unescape_line(parser, &line, line_number.unsigned_value, quote, 0, nullptr);
        if (uw_error(&unescaped)) {
            return uw_move(&unescaped);
        }
        UwValue status = uw_list_set_item(&lines, i, &unescaped);
        if (uw_error(&status)) {
            return uw_move(&status);
        }
    }}

    // return concatenated lines
    return uw_list_join(' ', &lines);
}

static UwResult parse_isodate(AmwParser* parser)
/*
 * Parse value as ISO-8601 date starting from block indent in the current line.
 * Return UwDateTime on success, UwStatus on error.
 */
{
    return UwStatus(UW_ERROR_NOT_IMPLEMENTED);
}

static UwResult parse_timestamp(AmwParser* parser)
/*
 * Parse value as ISO-8601 date starting from block indent in the current line.
 * Return UwTimestamp on success, UwStatus on error.
 */
{
    return UwStatus(UW_ERROR_NOT_IMPLEMENTED);
}

static UwResult parse_unsigned(AmwParser* parser, unsigned* pos, unsigned radix)
/*
 * Helper function for _amw_parse_number
 * Parse current line starting from `pos` as unsigned integer value.
 *
 * Return value and update `pos` where conversion has stopped.
 */
{
    UWDECL_Unsigned(result, 0);
    UwValuePtr current_line = &parser->current_line;
    bool digit_seen = false;
    bool separator_seen = false;
    unsigned p = *pos;
    for(;;) {
        char32_t chr = uw_char_at(current_line, p);

        // check separator
        if (chr == '\'' || chr == '_') {
            if (separator_seen) {
                return parser_error(parser, p, "Duplicate separator in the number");
            }
            if (!digit_seen) {
                return parser_error(parser, p, "Separator is not allowed in the beginning of number");
            }
            separator_seen = true;
            p++;
            if (end_of_line(current_line, p)) {
                return parser_error(parser, p, "Bad number");
            }
            continue;
        }
        separator_seen = false;

        // check digit and convert to number
        if (radix == 16) {
            if (chr >= 'a' && chr <= 'f') {
                chr -= 'a' - 10;
            } else if (chr >= 'A' && chr <= 'F') {
                chr -= 'A' - 10;
            } else if (chr >= '0' && chr <= '9') {
                chr -= '0';
            } else if (!digit_seen) {
                return parser_error(parser, p, "Bad number");
            } else {
                // not a digit, end of conversion
                *pos = p;
                return result;
            }
        } else if (chr >= '0' && chr < (char32_t) ('0' + radix)) {
            chr -= '0';
        } else if (!digit_seen) {
            return parser_error(parser, p, "Bad number");
        } else {
            // not a digit, end of conversion
            *pos = p;
            return result;
        }

        UwType_Unsigned prev_value = result.unsigned_value;
        result.unsigned_value *= radix;
        result.unsigned_value += chr;
        if (result.unsigned_value < prev_value) {
            return parser_error(parser, *pos, "Numeric overflow");
        }

        p++;
        if (end_of_line(current_line, p)) {
            // end of line, end of conversion
            *pos = p;
            return result;
        }
        digit_seen = true;
    }
}

static unsigned skip_digits(UwValuePtr str, unsigned pos)
{
    for (;;) {
        if (end_of_line(str, pos)) {
            break;
        }
        char32_t chr = uw_char_at(str, pos);
        if (!('0' <= chr && chr <= '9')) {
            break;
        }
        pos++;
    }
    return pos;
}

UwResult _amw_parse_number(AmwParser* parser, unsigned start_pos, int sign, unsigned* end_pos)
{
    TRACEPOINT();
    TRACE("start_pos %u", start_pos);

    UwValuePtr current_line = &parser->current_line;
    unsigned pos = start_pos;
    unsigned radix = 10;
    bool is_float = false;
    UWDECL_Unsigned(base, 0);
    UWDECL_Signed(result, 0);

    char32_t chr = uw_char_at(current_line, pos);
    if (chr == '0') {
        // check radix specifier
        pos++;
        if (end_of_line(current_line, pos)) {
            goto done;
        }
        switch (uw_char_at(current_line, pos)) {
            case 'b':
            case 'B':
                radix = 2;
                pos++;
                break;
            case 'o':
            case 'O':
                radix = 8;
                pos++;
                break;
            case 'x':
            case 'X':
                radix = 16;
                pos++;
                break;
            default:
                break;
        }
        if (end_of_line(current_line, pos)) {
            return parser_error(parser, start_pos, "Bad number");
        }
    }

    base = parse_unsigned(parser, &pos, radix);
    if (uw_error(&base)) {
        return uw_move(&base);
    }
    if (end_of_line(current_line, pos)) {
        goto done;
    }

    // check for fraction
    chr = uw_char_at(current_line, pos);
    if (chr == '.') {
        if (radix != 10) {
decimal_float_only:
            return parser_error(parser, start_pos, "Only decimal representation is supported for floating point numbers");
        }
        is_float = true;
        pos = skip_digits(current_line, pos + 1);
        if (end_of_line(current_line, pos)) {
            goto done;
        }
        chr = uw_char_at(current_line, pos);
    }
    // check for exponent
    if (chr == 'e' || chr == 'E') {
        if (radix != 10) {
            goto decimal_float_only;
        }
        is_float = true;
        pos++;
        if (end_of_line(current_line, pos)) {
            goto done;
        }
        chr = uw_char_at(current_line, pos);
        if (chr == '-' || chr == '+') {
            pos++;
        }
        pos = skip_digits(current_line, pos);

    } else if (chr != AMW_COMMENT && chr != ':' && !uw_isspace(chr)) {
        return parser_error(parser, start_pos, "Bad number");
    }

done:
    if (is_float) {
        // parse float
        unsigned len = pos - start_pos;
        char number[len + 1];
        uw_substrcopy_buf(current_line, start_pos, pos, number);
        errno = 0;
        double n = strtod(number, nullptr);
        if (errno == ERANGE) {
            return parser_error(parser, start_pos, "Floating point overflow");
        }
        if (sign < 0 && n != 0.0) {
            n = -n;
        }
        result = UwFloat(n);
    } else {
        // make integer
        if (base.unsigned_value > UW_SIGNED_MAX) {
            if (sign < 0) {
                return parser_error(parser, start_pos, "Integer overflow");
            } else {
                result = UwUnsigned(base.unsigned_value);
            }
        } else {
            if (sign < 0 && base.unsigned_value) {
                result = UwSigned(-base.unsigned_value);
            } else {
                result = UwSigned(base.unsigned_value);
            }
        }
    }
    *end_pos= pos;
    return uw_move(&result);
}

static bool comment_or_end_of_line(AmwParser* parser, unsigned position)
/*
 * Check if current line ends at position or contains comment.
 */
{
    position = uw_string_skip_spaces(&parser->current_line, position);
    return (end_of_line(&parser->current_line, position)
            || uw_char_at(&parser->current_line, position) == AMW_COMMENT);
}

static UwResult parse_list(AmwParser* parser)
/*
 * Parse list.
 *
 * Return list value on success.
 * Return nullptr on error.
 */
{
    TRACE_ENTER();

    UwValue result = UwList();
    if (uw_error(&result)) {
        return uw_move(&result);
    }

    /*
     * All list items must have the same indent.
     * Save indent of the first item (current one) and check it for subsequent items.
     */
    unsigned item_indent = get_start_position(parser);

    for (;;) {
        {
            // check if hyphen is followed by space or end of line
            unsigned next_pos = item_indent + 1;
            if (!isspace_or_eol_at(&parser->current_line, next_pos)) {
                return parser_error(parser, item_indent, "Bad list item");
            }

            // parse item as a nested block

            UwValue item = UwNull();
            if (comment_or_end_of_line(parser, next_pos)) {
                item = parse_nested_block_from_next_line(parser, value_parser_func);
            } else {
                // nested block starts on the same line, increment block position
                next_pos++;
                item = parse_nested_block(parser, next_pos, value_parser_func);
            }
            if (uw_error(&item)) {
                return uw_move(&item);;
            }
            if (!uw_list_append(&result, &item)) {
                return UwOOM();
            }

            UwValue status = _amw_read_block_line(parser);
            if (_amw_end_of_block(&status)) {
                break;
            }
            if (uw_error(&status)) {
                return uw_move(&status);
            }
            if (parser->current_indent != item_indent) {
                return parser_error(parser, parser->current_indent, "Bad indentation of list item");
            }
        }
    }
    TRACE_EXIT();
    return uw_move(&result);
}

static UwResult parse_map(AmwParser* parser, UwValuePtr first_key, unsigned value_pos)
/*
 * Parse map.
 *
 * Key is already parsed, continue parsing from `value_pos` in the `current_line`.
 *
 * Return map value on success.
 * Return status on error.
 */
{
    TRACE_ENTER();

    UwValue result = UwMap();
    if (uw_error(&result)) {
        return uw_move(&result);
    }

    UwValue key = uw_deepcopy(first_key);
    if (uw_error(&key)) {
        return uw_move(&key);
    }

    /*
     * All keys in the map must have the same indent.
     * Save indent of the first key (current one) and check it for subsequent keys.
     */
    unsigned key_indent = get_start_position(parser);

    for (;;) {
        TRACE("parse value from position %u", value_pos);
        {
            // parse value as a nested block

            UwValue value = UwNull();
            if (comment_or_end_of_line(parser, value_pos)) {
                value = parse_nested_block_from_next_line(parser, value_parser_func);
            } else {
                value = parse_nested_block(parser, value_pos, value_parser_func);
            }
            if (uw_error(&value)) {
                return uw_move(&value);
            }
            if (!uw_map_update(&result, &key, &value)) {
                return UwOOM();
            }
        }
        TRACE("parse next key");
        {
            uw_destroy(&key);

            UwValue status = _amw_read_block_line(parser);
            if (_amw_end_of_block(&status)) {
                break;
            }
            if (uw_error(&status)) {
                return uw_move(&status);
            }

            if (parser->current_indent != key_indent) {
                return parser_error(parser, parser->current_indent, "Bad indentation of map key");
            }

            key = parse_value(parser, &value_pos);
            if (uw_error(&key)) {
                return uw_move(&key);
            }
        }
    }
    TRACE_EXIT();
    return uw_move(&result);
}

static UwResult is_kv_separator(AmwParser* parser, unsigned colon_pos)
/*
 * Return UwBool(true) if colon_pos is followed by end of line, space, or conversion specifier.
 */
{
    if (end_of_line(&parser->current_line, colon_pos + 1)) {
        return UwBool(true);
    }
    char32_t chr = uw_char_at(&parser->current_line, colon_pos + 1);
    if (uw_isspace(chr)) {
        return UwBool(true);
    }
    if (chr != ':') {
        return UwBool(false);
    }
    unsigned value_pos;
    UwValue convspec = parse_convspec(parser, colon_pos, &value_pos);
    if (uw_error(&convspec)) {
        return uw_move(&convspec);
    }
    return UwBool(uw_is_string(&convspec));
}

static UwResult parse_literal_string_or_map(AmwParser* parser)
/*
 * Search key-value separator in the first line of current block.
 * If found, parse current block as a map. If not, parse as a literal string.
 */
{
    TRACEPOINT();

    unsigned start_pos = get_start_position(parser);

    // look for key-value separator
    unsigned colon_pos;
    if (uw_strchr(&parser->current_line, ':', start_pos, &colon_pos)) {
        UwValue kvs = is_kv_separator(parser, colon_pos);
        if (uw_error(&kvs)) {
            return uw_move(&kvs);
        }
        if (kvs.bool_value) {
            // found, parse map
            UwValue first_key = uw_substr(&parser->current_line, start_pos, colon_pos);
            if (!uw_string_trim(&first_key)) {
                return UwOOM();
            }
            return parse_map(parser, &first_key, colon_pos + 2);
        }
    }
    return parse_literal_string(parser);
}

static UwResult check_value_end(AmwParser* parser, UwValuePtr value, unsigned end_pos, unsigned* nested_value_pos)
/*
 * Helper function for parse_value.
 *
 * Check if value ends with key-value separator and parse map.
 * If not, check if end_pos points to end of line or comment.
 *
 * If `nested_value_pos` is provided, the value is _expected_ to be a map key
 * and _must_ end with key-value separator.
 *
 * On success return parsed value.
 * If `nested_value_pos' is provided, write position of the next char after colon to it.
 *
 * Read next line if nothing to parse on the current_line.
 *
 * Return cloned value.
 */
{
    //make sure value is not an error
    if (uw_error(value)) {
        return uw_clone(value);
    }

    end_pos = uw_string_skip_spaces(&parser->current_line, end_pos);
    if (end_of_line(&parser->current_line, end_pos)) {
        if (nested_value_pos) {
            return parser_error(parser, end_pos, "Map key expected");
        }
        // read next line
        UwValue status = _amw_read_block_line(parser);
        if (!_amw_end_of_block(&status)) {
            if (uw_error(&status)) {
                return uw_move(&status);
            }
        }
        return uw_clone(value);
    }

    char32_t chr = uw_char_at(&parser->current_line, end_pos);
    if (chr == ':') {
        UwValue kvs = is_kv_separator(parser, end_pos);
        if (uw_error(&kvs)) {
            return uw_move(&kvs);
        }
        if (kvs.bool_value) {
            // found key-value separator
            if (nested_value_pos) {
                // it was expected, just return value
                *nested_value_pos = end_pos + 1;
                return uw_clone(value);
            }
            // parse map
            UwValue first_key = uw_clone(value);
            return parse_map(parser, &first_key, end_pos + 2);
        }
        return parser_error(parser, end_pos + 1, "Bad character encountered");
    }

    if (chr != AMW_COMMENT) {
        return parser_error(parser, end_pos, "Bad character encountered");
    }

    // read next line
    UwValue status = _amw_read_block_line(parser);
    if (!_amw_end_of_block(&status)) {
        if (uw_error(&status)) {
            return uw_move(&status);
        }
    }
    return uw_clone(value);
}

static UwResult parse_value(AmwParser* parser, unsigned* nested_value_pos)
/*
 * Parse value starting from `current_line[block_indent]` .
 *
 * If `nested_value_pos` is provided, the value is _expected_ to be a map key
 * and _must_ end with colon or include a colon if it's a literal strings.
 *
 * On success return parsed value.
 * If `nested_value_pos' is provided, write position of the next char after colon to it.
 *
 * On error return status and set `parser->result["error"]`.
 */
{
    TRACEPOINT();

    unsigned start_pos = get_start_position(parser);

    // Analyze first character.
    char32_t chr = uw_char_at(&parser->current_line, start_pos);

    // first, check if value starts with colon that may denote conversion specifier

    if (chr == ':') {
        // this might be conversion specifier
        if (nested_value_pos) {
            // we expect map key, and map keys cannot start with colon
            // because they would look same as conversion specifier
            return parser_error(parser, start_pos, "Map key expected and it cannot start with colon");
        }
        unsigned value_pos;
        UwValue convspec = parse_convspec(parser, start_pos, &value_pos);
        if (uw_error(&convspec)) {
            return uw_move(&convspec);
        }
        if (!uw_is_string(&convspec)) {
            // not a conversion specifier
            return parse_literal_string(parser);
        }
        // we have conversion specifier
        if (end_of_line(&parser->current_line, value_pos)) {
            return parse_nested_block_from_next_line(
                parser, get_custom_parser(parser, &convspec)
            );
        } else {
            return parse_nested_block(
                parser, value_pos, get_custom_parser(parser, &convspec)
            );
        }
    }

    // other values can be map keys

    // check for dash

    if (chr == '-') {
        unsigned next_pos = start_pos + 1;
        char32_t next_chr = uw_char_at(&parser->current_line, next_pos);

        // if followed by digit, it's a number
        if ('0' <= next_chr && next_chr <= '9') {
            unsigned end_pos;
            UwValue number = _amw_parse_number(parser, next_pos, -1, &end_pos);
            return check_value_end(parser, &number, end_pos, nested_value_pos);
        }
        // if followed by space or end of line, that's a list item
        if (isspace_or_eol_at(&parser->current_line, next_pos)) {
            if (nested_value_pos) {
                return parser_error(parser, start_pos, "Map key expected and it cannot be a list");
            }
            // yes, it's a list item
            return parse_list(parser);
        }
        // otherwise, it's a literal string or map
        return parse_literal_string_or_map(parser);
    }

    // check for quoted string

    if (chr == '"' || chr == '\"') {
        // quoted string
        unsigned start_line = parser->line_number;
        unsigned end_pos;
        UwValue str = parse_quoted_string(parser, start_pos, &end_pos);
        if (uw_error(&str)) {
            return uw_move(&str);
        }
        unsigned end_line = parser->line_number;
        if (end_line == start_line) {
            // single-line string can be a map key
            return check_value_end(parser, &str, end_pos, nested_value_pos);
        } else if (comment_or_end_of_line(parser, end_pos)) {
            // multi-line string cannot be a key
            return uw_move(&str);
        } else {
            return parser_error(parser, end_pos, "Bad character after quoted string");
        }
    }

    // check for reserved keywords

    TRACE("trying reserved keywords");
    if (uw_substring_eq_cstr(&parser->current_line, start_pos, start_pos + 4, "null")) {
        UwValue null_value = UwNull();
        return check_value_end(parser, &null_value, start_pos + 4, nested_value_pos);
    }
    if (uw_substring_eq_cstr(&parser->current_line, start_pos, start_pos + 4, "true")) {
        UwValue true_value = UwBool(true);
        return check_value_end(parser, &true_value, start_pos + 4, nested_value_pos);
    }
    if (uw_substring_eq_cstr(&parser->current_line, start_pos, start_pos + 5, "false")) {
        UwValue false_value = UwBool(false);
        return check_value_end(parser, &false_value, start_pos + 5, nested_value_pos);
    }

    // try parsing number

    TRACE("not a keyword, trying number");
    if (chr == '+') {
        char32_t next_chr = uw_char_at(&parser->current_line, start_pos + 1);
        if ('0' <= next_chr && next_chr <= '9') {
            start_pos++;
            chr = next_chr;
        }
    }
    if ('0' <= chr && chr <= '9') {
        unsigned end_pos;
        UwValue number = _amw_parse_number(parser, start_pos, 1, &end_pos);
        return check_value_end(parser, &number, end_pos, nested_value_pos);
    }

    // parsed none of above, then

    return parse_literal_string_or_map(parser);
}

static UwResult value_parser_func(AmwParser* parser)
{
    return parse_value(parser, nullptr);
}

UwResult amw_parse(UwValuePtr markup)
{
    [[ gnu::cleanup(amw_delete_parser) ]] AmwParser* parser = amw_create_parser(markup);
    if (!parser) {
        return UwOOM();
    }
    // read first line to prepare for parsing and to detect EOF
    UwValue status = _amw_read_block_line(parser);
    if (_amw_end_of_block(&status) && parser->eof) {
        return UwStatus(UW_ERROR_EOF);
    }
    if (uw_error(&status)) {
        return uw_move(&status);
    }
    // parse top-level value
    UwValue result = value_parser_func(parser);
    if (uw_error(&result)) {
        return uw_move(&result);
    }
    // make sure markup has no more data
    status = _amw_read_block_line(parser);
    if (parser->eof) {
        // all right, no op
    } else {
        if (uw_error(&status)) {
            return uw_move(&status);
        }
        return parser_error(parser, parser->current_indent, "Extra data after parsed value");
    }
    return uw_move(&result);
}
