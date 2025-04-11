#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

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
static UwResult parse_value(AmwParser* parser, unsigned* nested_value_pos, UwValuePtr convspec);
static UwResult value_parser_func(AmwParser* parser);
static UwResult parse_raw_value(AmwParser* parser);
static UwResult parse_literal_string(AmwParser* parser);
static UwResult parse_folded_string(AmwParser* parser);
static UwResult parse_datetime(AmwParser* parser);
static UwResult parse_timestamp(AmwParser* parser);

static char number_terminators[] = { AMW_COMMENT, ':', 0 };


AmwParser* amw_create_parser(UwValuePtr markup)
{
    AmwParser* parser = allocate(sizeof(AmwParser), true);
    if (!parser) {
        return nullptr;
    }
    parser->markup = uw_clone(markup);

    parser->blocklevel = 1;
    parser->max_blocklevel = AMW_MAX_RECURSION_DEPTH;

    parser->json_depth = 1;
    parser->max_json_depth = AMW_MAX_RECURSION_DEPTH;

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
        UwCharPtr("datetime"),  UwPtr((void*) parse_datetime),
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

UwResult _amw_parser_error(AmwParser* parser, char* source_file_name, unsigned source_line_number,
                           unsigned line_number, unsigned char_pos, char* description, ...)
{
    UwValue status = uw_create(UwTypeId_AmwStatus);
    // status is UW_SUCCESS by default
    // can't use uw_if_error here because of simplified checking in uw_ok
    if (status.status_code != UW_SUCCESS) {
        return uw_move(&status);
    }

    status.status_code = AMW_PARSE_ERROR;
    _uw_set_status_location(&status, source_file_name, source_line_number);
    AmwStatusData* status_data = _amw_status_data_ptr(&status);
    status_data->line_number = line_number;;
    status_data->position = char_pos;

    va_list ap;
    va_start(ap);
    _uw_set_status_desc_ap(&status, description, ap);
    va_end(ap);
    return uw_move(&status);
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
    uw_return_if_error(&status);

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
        uw_return_if_error(&status);

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

    UwValue lines = UwArray();
    uw_return_if_error(&lines);

    for (;;) {{
        // append line
        UwValue line = uw_substr(&parser->current_line, parser->block_indent, UINT_MAX);
        uw_return_if_error(&line);

        if (!uw_array_append(&lines, &line)) {
            return UwOOM();
        }
        // read next line
        UwValue status = _amw_read_block_line(parser);
        if (_amw_end_of_block(&status)) {
            return uw_move(&lines);
        }
        uw_return_if_error(&status);
    }}
}

static UwResult parse_nested_block(AmwParser* parser, unsigned block_pos, AmwBlockParserFunc parser_func)
/*
 * Set block indent to `block_pos` and call parser_func.
 */
{
    if (parser->blocklevel >= parser->max_blocklevel) {
        return amw_parser_error(parser, parser->current_indent, "Too many nested blocks");
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
    parser->skip_comments = true;
    UwValue status = _amw_read_block_line(parser);
    parser->block_indent--;

    if (_amw_end_of_block(&status)) {
        return amw_parser_error(parser, parser->current_indent, "Empty block");
    }
    uw_return_if_error(&status);

    // call parse_nested_block
    return parse_nested_block(parser, parser->block_indent + 1, parser_func);
}

unsigned _amw_get_start_position(AmwParser* parser)
{
    if (parser->block_indent < parser->current_indent) {
        return parser->current_indent;
    } else {
        return uw_string_skip_spaces(&parser->current_line, parser->block_indent);
    }
}

bool _amw_comment_or_end_of_line(AmwParser* parser, unsigned position)
{
    position = uw_string_skip_spaces(&parser->current_line, position);
    return (end_of_line(&parser->current_line, position)
            || uw_char_at(&parser->current_line, position) == AMW_COMMENT);
}

static UwResult parse_convspec(AmwParser* parser, unsigned opening_colon_pos, unsigned* end_pos)
/*
 * Extract conversion specifier starting from `opening_colon_pos` in the `current_line`.
 *
 * On success return string and write `end_pos`.
 *
 * If conversion specified is not detected, return UwNull()
 *
 * On error return UwStatus.
 */
{
    UwValuePtr current_line = &parser->current_line;

    unsigned start_pos = opening_colon_pos + 1;
    unsigned closing_colon_pos;
    if (!uw_strchr(current_line, ':', start_pos, &closing_colon_pos)) {
        return UwNull();
    }
    if (closing_colon_pos == start_pos) {
        // empty conversion specifier
        return UwNull();
    }
    if (!isspace_or_eol_at(current_line, closing_colon_pos + 1)) {
        // not a conversion specifier
        return UwNull();
    }
    UwValue convspec = uw_substr(current_line, start_pos, closing_colon_pos);
    uw_return_if_error(&convspec);

    if (!uw_string_trim(&convspec)) {
        return UwOOM();
    }
    if (!have_custom_parser(parser, &convspec)) {
        // such a conversion specifier is not defined
        return UwNull();
    }
    *end_pos = closing_colon_pos + 1;
    return uw_move(&convspec);
}

static UwResult parse_raw_value(AmwParser* parser)
{
    TRACEPOINT();

    UwValue lines = _amw_read_block(parser);
    uw_return_if_error(&lines);

    if (uw_array_length(&lines) > 1) {
        // append one empty line for ending line break
        UWDECL_String(empty_line);
        if (!uw_array_append(&lines, &empty_line)) {
            return UwOOM();
        }
    }
    // return concatenated lines
    return uw_array_join('\n', &lines);
}

static UwResult parse_literal_string(AmwParser* parser)
/*
 * Parse current block as a literal string.
 */
{
    TRACEPOINT();

    UwValue lines = _amw_read_block(parser);
    uw_return_if_error(&lines);

    // normalize list of lines

    if (!uw_array_dedent(&lines)) {
        return UwOOM();
    }
    // drop empty trailing lines
    unsigned len = uw_array_length(&lines);
    while (len--) {{
        UwValue line = uw_array_item(&lines, len);
        if (uw_strlen(&line) != 0) {
            break;
        }
        uw_array_del(&lines, len, len + 1);
    }}

    // append one empty line for ending line break
    if (uw_array_length(&lines) > 1) {
        UWDECL_String(empty_line);
        if (!uw_array_append(&lines, &empty_line)) {
            return UwOOM();
        }
    }

    // return concatenated lines
    return uw_array_join('\n', &lines);
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
        len - start_pos,  // unescaped string can be shorter
        uw_string_char_size(line)
    );
    unsigned pos = start_pos;
    while (pos < len) {
        char32_t chr = uw_char_at(line, pos);
        if (chr == quote) {
            // closing quotation mark detected
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
                                return amw_parser_error2(parser, line_number, pos, "Incomplete octal value");
                            }
                            break;
                        }
                        char32_t c = uw_char_at(line, pos);
                        if ('0' <= c && c <= '7') {
                            v <<= 3;
                            v += c - '0';
                        } else {
                            return amw_parser_error2(parser, line_number, pos, "Bad octal value");
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
                            return amw_parser_error2(parser, line_number, pos, "Incomplete hexadecimal value");
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
                            return amw_parser_error2(parser, line_number, pos, "Bad hexadecimal value");
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

static UwResult fold_lines(AmwParser* parser, UwValuePtr lines, char32_t quote, UwValuePtr line_numbers)
/*
 * Fold list of lines and return concatenated string.
 *
 * If `quote` is nonzero, unescape lines.
 */
{
    if (!uw_array_dedent(lines)) {
        return UwOOM();
    }
    unsigned len = uw_array_length(lines);

    // skip leading empty lines
    unsigned start_i = 0;
    for (; start_i < len; start_i++) {{
        UwValue line = uw_array_item(lines, start_i);
        if (uw_strlen(&line) != 0) {
            break;
        }
    }}
    if (start_i == len) {
        // return empty string
        return UwString();
    }

    // skip trailing empty lines
    unsigned end_i = len;
    for (; end_i; end_i--) {{
        UwValue line = uw_array_item(lines, end_i - 1);
        if (uw_strlen(&line) != 0) {
            break;
        }
    }}
    if (end_i == 0) {
        // return empty string
        return UwString();
    }

    // calculate length of result
    unsigned result_len = end_i - start_i - 1;  // reserve space for separators
    uint8_t char_size = 1;
    for (unsigned i = start_i; i < end_i; i++) {{
        UwValue line = uw_array_item(lines, i);
        result_len += uw_strlen(&line);
        uint8_t cs = uw_string_char_size(&line);
        if (cs > char_size) {
            char_size = cs;
        }
    }}

    // allocate result
    UwValue result = uw_create_empty_string(result_len, char_size);
    uw_return_if_error(&result);

    // concatenate lines
    bool prev_LF = false;
    for (unsigned i = start_i; i < end_i; i++) {{
        UwValue line = uw_array_item(lines, i);
        if (i > start_i) {
            if (uw_strlen(&line) == 0) {
                // treat empty lines as LF
                if (!uw_string_append(&line, '\n')) {
                    return UwOOM();
                }
                prev_LF = true;
            } else {
                if (prev_LF) {
                    // do not append separator if previous line was empty
                    prev_LF = false;
                } else {
                    if (uw_isspace(uw_char_at(&line, 0))) {
                        // do not append separator if the line aleady starts with space
                    } else {
                        if (!uw_string_append(&result, ' ')) {
                            return UwOOM();
                        }
                    }
                }
            }
        }
        if (quote) {
            UwValue line_number = uw_array_item(line_numbers, i);
            UwValue unescaped = _amw_unescape_line(parser, &line, line_number.unsigned_value, quote, 0, nullptr);
            uw_return_if_error(&unescaped);
            if (!uw_string_append(&result, &unescaped)) {
                return UwOOM();
            }
        } else {
            if (!uw_string_append(&result, &line)) {
                return UwOOM();
            }
        }
    }}
    return uw_move(&result);
}

static UwResult parse_folded_string(AmwParser* parser)
{
    TRACEPOINT();

    UwValue lines = _amw_read_block(parser);
    uw_return_if_error(&lines);

    return fold_lines(parser, &lines, 0, nullptr);
}

bool _amw_find_closing_quote(UwValuePtr line, char32_t quote, unsigned start_pos, unsigned* end_pos)
{
    for (;;) {
        if (!uw_strchr(line, quote, start_pos, end_pos)) {
            return false;
        }
        // check if the quotation mark is not escaped
        if (*end_pos && uw_char_at(line, *end_pos - 1) == '\\') {
            // continue searching
            start_pos = *end_pos + 1;
        } else {
            return true;
        }
    }
}

static UwResult parse_quoted_string(AmwParser* parser, unsigned opening_quote_pos, unsigned* end_pos)
/*
 * Parse quoted string starting from `opening_quote_pos` in the current line.
 *
 * Write next position after the closing quotation mark to `end_pos`.
 */
{
    TRACEPOINT();

    // Get opening quote. The closing quote should be the same.
    char32_t quote = uw_char_at(&parser->current_line, opening_quote_pos);

    // process first line
    if (_amw_find_closing_quote(&parser->current_line, quote, opening_quote_pos + 1, end_pos)) {
        // single-line string
        (*end_pos)++;
        return _amw_unescape_line(parser, &parser->current_line, parser->line_number,
                                  quote, opening_quote_pos + 1, nullptr);
    }

    unsigned block_indent = opening_quote_pos + 1;

    // make parser read nested block
    unsigned saved_block_indent = parser->block_indent;
    parser->block_indent = block_indent;
    parser->blocklevel++;

    // read block
    UwValue lines = UwArray();
    uw_return_if_error(&lines);

    UwValue line_numbers = UwArray();
    uw_return_if_error(&line_numbers);

    bool closing_quote_detected = false;
    for (;;) {{
        // append line number
        UwValue n = UwUnsigned(parser->line_number);
        if (!uw_array_append(&line_numbers, &n)) {
            return UwOOM();
        }
        // append line
        if (_amw_find_closing_quote(&parser->current_line, quote, block_indent, end_pos)) {
            // final line
            UwValue final_line = uw_substr(&parser->current_line, block_indent, *end_pos);
            if (!uw_string_rtrim(&final_line)) {
                return UwOOM();
            }
            if (!uw_array_append(&lines, &final_line)) {
                return UwOOM();
            }
            (*end_pos)++;
            closing_quote_detected = true;
            break;
        } else {
            // intermediate line
            UwValue line = uw_substr(&parser->current_line, block_indent, UINT_MAX);
            uw_return_if_error(&line);
            if (!uw_array_append(&lines, &line)) {
                return UwOOM();
            }
        }
        // read next line
        UwValue status = _amw_read_block_line(parser);
        if (_amw_end_of_block(&status)) {
            break;
        }
        uw_return_if_error(&status);
    }}

    // finished reading nested block
    parser->block_indent = saved_block_indent;
    parser->blocklevel--;

    if (!closing_quote_detected) {

        static char unterminated[] = "String has no closing quote";

        // the above loop terminated abnormally, need to read next line
        UwValue status = _amw_read_block_line(parser);
        if (_amw_end_of_block(&status)) {
            return amw_parser_error(parser, parser->current_indent, unterminated);
        }
        // check if the line starts with a quote with the same indent as the opening quote
        if (parser->current_indent == opening_quote_pos
            && uw_char_at(&parser->current_line, parser->current_indent) == quote) {

            *end_pos = opening_quote_pos + 1;
        } else {
            return amw_parser_error(parser, parser->current_indent, unterminated);
        }
    }

    // fold and unescape

    return fold_lines(parser, &lines, quote, &line_numbers);
}

static bool parse_nanosecond_frac(AmwParser* parser, unsigned* pos, uint32_t* result)
/*
 * Parse fractional nanoseconds part in the current line starting from `pos`.
 * Always update `pos` upon return.
 * Return true on success and write parsed value to `result`.
 * On error return false.
 */
{
    unsigned p = *pos;
    uint32_t nanoseconds = 0;
    unsigned i = 0;
    while (!end_of_line(&parser->current_line, p)) {
        char32_t chr = uw_char_at(&parser->current_line, p);
        if (!uw_isdigit(chr)) {
            break;
        }
        if (i == 9) {
            *pos = p;
            return false;
        }
        nanoseconds *= 10;
        nanoseconds += chr - '0';
        i++;
        p++;
    }
    if (i == 0) {
    }
    static unsigned order[] = {
        1000'000'000,  // unused, i starts from 1 here
        100'000'000,
        10'000'000,
        1000'000,
        100'000,
        10'000,
        1000,
        100,
        10,
        1
    };
    *result = nanoseconds * order[i];
    *pos = p;
    return true;
}

static UwResult parse_datetime(AmwParser* parser)
/*
 * Parse value date/time starting from block indent in the current line.
 * Return UwDateTime on success, UwStatus on error.
 */
{
    static char bad_datetime[] = "Bad date/time";
    UWDECL_DateTime(result);
    UwValuePtr current_line = &parser->current_line;
    unsigned pos = _amw_get_start_position(parser);
    char32_t chr;

    // parse YYYY part
    for (unsigned i = 0; i < 4; i++, pos++) {
        chr = uw_char_at(current_line, pos);
        if (!uw_isdigit(chr)) { return amw_parser_error(parser, pos, bad_datetime); }
        result.year *= 10;
        result.year += chr - '0';
    }
    // skip optional separator
    if (uw_char_at(current_line, pos) == '-') {
        pos++;
    }
    // parse MM part
    for (unsigned i = 0; i < 2; i++, pos++) {
        chr = uw_char_at(current_line, pos);
        if (!uw_isdigit(chr)) { return amw_parser_error(parser, pos, bad_datetime); }
        result.month *= 10;
        result.month += chr - '0';
    }
    // skip optional separator
    if (uw_char_at(current_line, pos) == '-') {
        pos++;
    }
    // parse DD part
    for (unsigned i = 0; i < 2; i++, pos++) {
        chr = uw_char_at(current_line, pos);
        if (!uw_isdigit(chr)) { return amw_parser_error(parser, pos, bad_datetime); }
        result.day *= 10;
        result.day += chr - '0';
    }
    // skip optional separator
    chr = uw_char_at(current_line, pos);
    if (chr == 'T') {
        pos++;
    } else {
        pos = uw_string_skip_spaces(current_line, pos);
        if (end_of_line(current_line, pos)) { goto end_of_datetime; }
        chr = uw_char_at(current_line, pos);
        if (chr == AMW_COMMENT) { goto end_of_datetime; }
    }
    // parse HH part
    for (unsigned i = 0; i < 2; i++, pos++) {
        chr = uw_char_at(current_line, pos);
        if (!uw_isdigit(chr)) { return amw_parser_error(parser, pos, bad_datetime); }
        result.hour *= 10;
        result.hour += chr - '0';
    }
    // skip optional separator
    if (uw_char_at(current_line, pos) == ':') {
        pos++;
    }
    // parse MM part
    for (unsigned i = 0; i < 2; i++, pos++) {
        chr = uw_char_at(current_line, pos);
        if (!uw_isdigit(chr)) { return amw_parser_error(parser, pos, bad_datetime); }
        result.minute *= 10;
        result.minute += chr - '0';
    }
    // skip optional separator
    if (uw_char_at(current_line, pos) == ':') {
        pos++;
    }
    // parse SS part
    for (unsigned i = 0; i < 2; i++, pos++) {
        chr = uw_char_at(current_line, pos);
        if (!uw_isdigit(chr)) { return amw_parser_error(parser, pos, bad_datetime); }
        result.second *= 10;
        result.second += chr - '0';
    }
    // check optional parts
    chr = uw_char_at(current_line, pos);
    if (chr == 'Z') {
        pos++;
        goto end_of_datetime;
    }
    if ( chr == '.') {
        // parse nanoseconds
        pos++;
        if (!parse_nanosecond_frac(parser, &pos, &result.nanosecond)) {
            return amw_parser_error(parser, pos, bad_datetime);
        }
        chr = uw_char_at(current_line, pos);
    }
    if (chr == 'Z') {
        pos++;

    } else if (chr == '+' || chr == '-') {
        // parse GMT offset
        int sign = (chr == '-')? -1 : 1;
        pos++;
        // parse HH part
        unsigned offset_hour = 0;
        for (unsigned i = 0; i < 2; i++, pos++) {
            chr = uw_char_at(current_line, pos);
            if (!uw_isdigit(chr)) { return amw_parser_error(parser, pos, bad_datetime); }
            offset_hour *= 10;
            offset_hour += chr - '0';
        }
        // skip optional separator
        if (uw_char_at(current_line, pos) == ':') {
            pos++;
        }
        // parse optional MM part
        unsigned offset_minute = 0;
        if (!end_of_line(current_line, pos)) {
            chr = uw_char_at(current_line, pos);
            if (uw_isdigit(chr)) {
                for (unsigned i = 0; i < 2; i++, pos++) {
                    chr = uw_char_at(current_line, pos);
                    if (!uw_isdigit(chr)) { return amw_parser_error(parser, pos, bad_datetime); }
                    offset_minute *= 10;
                    offset_minute += chr - '0';
                }
            }
        }
        result.gmt_offset = sign * offset_hour * 60 + offset_minute;
    }

end_of_datetime:
    pos = uw_string_skip_spaces(current_line, pos);
    if (!end_of_line(current_line, pos)) {
        chr = uw_char_at(current_line, pos);
        if (chr != AMW_COMMENT) { return amw_parser_error(parser, pos, bad_datetime); }
    }
    return uw_move(&result);
}

static UwResult parse_unsigned(AmwParser* parser, unsigned* pos, unsigned radix)
/*
 * Helper function for _amw_parse_number and parse_timestamp.
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
                return amw_parser_error(parser, p, "Duplicate separator in the number");
            }
            if (!digit_seen) {
                return amw_parser_error(parser, p, "Separator is not allowed in the beginning of number");
            }
            separator_seen = true;
            p++;
            if (end_of_line(current_line, p)) {
                return amw_parser_error(parser, p, "Bad number");
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
                return amw_parser_error(parser, p, "Bad number");
            } else {
                // not a digit, end of conversion
                *pos = p;
                return result;
            }
        } else if (chr >= '0' && chr < (char32_t) ('0' + radix)) {
            chr -= '0';
        } else if (!digit_seen) {
            return amw_parser_error(parser, p, "Bad number");
        } else {
            // not a digit, end of conversion
            *pos = p;
            return result;
        }
        if (result.unsigned_value > UW_UNSIGNED_MAX / radix) {
            // overflow
            return amw_parser_error(parser, *pos, "Numeric overflow");
        }
        UwType_Unsigned new_value = result.unsigned_value * radix + chr;
        if (new_value < result.unsigned_value) {
            // overflow
            return amw_parser_error(parser, *pos, "Numeric overflow");
        }
        result.unsigned_value = new_value;

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

static UwResult parse_timestamp(AmwParser* parser)
/*
 * Parse value as timestamp starting from block indent in the current line.
 * Return UwTimestamp on success, UwStatus on error.
 */
{
    static char bad_timestamp[] = "Bad timestamp";
    UWDECL_Timestamp(result);
    unsigned pos = _amw_get_start_position(parser);

    UwValue seconds = parse_unsigned(parser, &pos, 10);
    uw_return_if_error(&seconds);

    result.ts_seconds = seconds.unsigned_value;

    if (end_of_line(&parser->current_line, pos)) {
        return uw_move(&result);
    }

    char32_t chr = uw_char_at(&parser->current_line, pos);
    if ( chr == '.') {
        // parse nanoseconds
        pos++;
        if (!parse_nanosecond_frac(parser, &pos, &result.ts_nanoseconds)) {
            return amw_parser_error(parser, pos, bad_timestamp);
        }
    }
    if (_amw_comment_or_end_of_line(parser, pos)) {
        return uw_move(&result);
    } else {
        return amw_parser_error(parser, pos, bad_timestamp);
    }
}

UwResult _amw_parse_number(AmwParser* parser, unsigned start_pos, int sign, unsigned* end_pos, char* allowed_terminators)
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
        if (end_of_line(current_line, pos)) {
            goto done;
        }
        switch (uw_char_at(current_line, pos + 1)) {
            case 'b':
            case 'B':
                radix = 2;
                pos += 2;
                break;
            case 'o':
            case 'O':
                radix = 8;
                pos += 2;
                break;
            case 'x':
            case 'X':
                radix = 16;
                pos += 2;
                break;
            default:
                break;
        }
        if (end_of_line(current_line, pos)) {
            return amw_parser_error(parser, start_pos, "Bad number");
        }
    }

    base = parse_unsigned(parser, &pos, radix);
    uw_return_if_error(&base);

    if (end_of_line(current_line, pos)) {
        goto done;
    }

    // check for fraction
    chr = uw_char_at(current_line, pos);
    if (chr == '.') {
        if (radix != 10) {
decimal_float_only:
            return amw_parser_error(parser, start_pos, "Only decimal representation is supported for floating point numbers");
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
        unsigned next_pos = skip_digits(current_line, pos);
        if (next_pos == pos) {
            return amw_parser_error(parser, start_pos, "Bad exponent");
        }
        pos = next_pos;

    } else if ( ! (uw_isspace(chr) || strchr(allowed_terminators, chr))) {
fprintf(stderr, "XXX `%c`\n", chr);
        return amw_parser_error(parser, start_pos, "Bad number");
    }

done:
    if (is_float) {
        // parse float
        unsigned len = pos - start_pos;
        char number[len + 1];
        uw_substr_to_utf8_buf(current_line, start_pos, pos, number);
        errno = 0;
        double n = strtod(number, nullptr);
        if (errno == ERANGE) {
            return amw_parser_error(parser, start_pos, "Floating point overflow");
        } else if (errno) {
            return amw_parser_error(parser, start_pos, "Floating point conversion error");
        }
        if (sign < 0 && n != 0.0) {
            n = -n;
        }
        result = UwFloat(n);
    } else {
        // make integer
        if (base.unsigned_value > UW_SIGNED_MAX) {
            if (sign < 0) {
                return amw_parser_error(parser, start_pos, "Integer overflow");
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

static UwResult parse_list(AmwParser* parser)
/*
 * Parse list.
 *
 * Return list value on success.
 * Return nullptr on error.
 */
{
    TRACE_ENTER();

    UwValue result = UwArray();
    uw_return_if_error(&result);

    /*
     * All list items must have the same indent.
     * Save indent of the first item (current one) and check it for subsequent items.
     */
    unsigned item_indent = _amw_get_start_position(parser);

    for (;;) {
        {
            // check if hyphen is followed by space or end of line
            unsigned next_pos = item_indent + 1;
            if (!isspace_or_eol_at(&parser->current_line, next_pos)) {
                return amw_parser_error(parser, item_indent, "Bad list item");
            }

            // parse item as a nested block

            UwValue item = UwNull();
            if (_amw_comment_or_end_of_line(parser, next_pos)) {
                item = parse_nested_block_from_next_line(parser, value_parser_func);
            } else {
                // nested block starts on the same line, increment block position
                next_pos++;
                item = parse_nested_block(parser, next_pos, value_parser_func);
            }
            uw_return_if_error(&item);

            if (!uw_array_append(&result, &item)) {
                return UwOOM();
            }

            UwValue status = _amw_read_block_line(parser);
            if (_amw_end_of_block(&status)) {
                break;
            }
            uw_return_if_error(&status);

            if (parser->current_indent != item_indent) {
                return amw_parser_error(parser, parser->current_indent, "Bad indentation of list item");
            }
        }
    }
    TRACE_EXIT();
    return uw_move(&result);
}

static UwResult parse_map(AmwParser* parser, UwValuePtr first_key, UwValuePtr convspec_arg, unsigned value_pos)
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
    uw_return_if_error(&result);

    UwValue key = uw_clone(first_key);
    UwValue convspec = uw_clone(convspec_arg);

    /*
     * All keys in the map must have the same indent.
     * Save indent of the first key (current one) and check it for subsequent keys.
     */
    unsigned key_indent = _amw_get_start_position(parser);

    for (;;) {
        TRACE("parse value (line %u) from position %u", parser->line_number, value_pos);
        {
            // parse value as a nested block

            AmwBlockParserFunc parser_func = value_parser_func;
            if (uw_is_string(&convspec)) {
                parser_func = get_custom_parser(parser, &convspec);
            }
            UwValue value = UwNull();
            if (_amw_comment_or_end_of_line(parser, value_pos)) {
                value = parse_nested_block_from_next_line(parser, parser_func);

            } else {
                value = parse_nested_block(parser, value_pos, parser_func);
            }
            uw_return_if_error(&value);

            if (!uw_map_update(&result, &key, &value)) {
                return UwOOM();
            }
        }
        TRACE("parse next key");
        {
            uw_destroy(&key);
            uw_destroy(&convspec);

            UwValue status = _amw_read_block_line(parser);
            if (_amw_end_of_block(&status)) {
                TRACE("end of map");
                break;
            }
            uw_return_if_error(&status);

            if (parser->current_indent != key_indent) {
                return amw_parser_error(parser, parser->current_indent, "Bad indentation of map key");
            }

            key = parse_value(parser, &value_pos, &convspec);
            uw_return_if_error(&key);
        }
    }
    TRACE_EXIT();
    return uw_move(&result);
}

static UwResult is_kv_separator(AmwParser* parser, unsigned colon_pos,
                                UwValuePtr convspec_out, unsigned *value_pos)
/*
 * Return UwBool(true) if colon_pos is followed by end of line, space, or conversion specifier.
 * Write conversion specifier to `convspec_out` if value is followed by conversion specifier.
 * Write position of value to value_pos.
 */
{
    UwValuePtr current_line = &parser->current_line;

    unsigned next_pos = colon_pos + 1;

    if (end_of_line(current_line, next_pos)) {
        *value_pos = next_pos;
        return UwBool(true);
    }
    char32_t chr = uw_char_at(current_line, next_pos);
    if (isspace(chr)) {
        *value_pos = next_pos + 1;  // value should be separated from key by at least one space
        next_pos = uw_string_skip_spaces(current_line, next_pos);
        // cannot be end of line here because current line is R-trimmed and EOL is already checked
        chr = uw_char_at(current_line, next_pos);
        if (chr != ':') {
            // separator without conversion specifier
            return UwBool(true);
        }
    } else if (chr != ':') {
        // key not followed immediately by conversion specifier -> not a separator
        return UwBool(false);
    }

    // try parsing conversion specifier
    // value_pos will be updated only if conversion specifier is valid
    UwValue convspec = parse_convspec(parser, next_pos, value_pos);
    uw_return_if_error(&convspec);

    if (uw_is_string(&convspec)) {
        if (convspec_out) {
            *convspec_out = uw_move(&convspec);
        }
        return UwBool(true);
    }

    // bad conversion specifier -> not a separator
    return UwBool(false);
}

static UwResult check_value_end(AmwParser* parser, UwValuePtr value, unsigned end_pos,
                                unsigned* nested_value_pos, UwValuePtr convspec_out)
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
 * If `nested_value_pos' is not null, write position of the next char after colon to it
 * and write conversion specifier to `convspec_out` if value is followed by conversion specifier.
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
            return amw_parser_error(parser, end_pos, "Map key expected");
        }
        // read next line
        UwValue status = _amw_read_block_line(parser);
        if (!_amw_end_of_block(&status)) {
            uw_return_if_error(&status);
        }
        return uw_clone(value);
    }

    char32_t chr = uw_char_at(&parser->current_line, end_pos);
    if (chr == ':') {
        // check key-value separator
        UwValue convspec = UwNull();
        unsigned value_pos;
        UwValue kvs = is_kv_separator(parser, end_pos, &convspec, &value_pos);
        uw_return_if_error(&kvs);

        if (kvs.bool_value) {
            // found key-value separator
            if (nested_value_pos) {
                // it was anticipated, just return the value
                *nested_value_pos = value_pos;
                *convspec_out = uw_move(&convspec);
                return uw_clone(value);
            }
            // parse map
            UwValue first_key = uw_clone(value);
            return parse_map(parser, &first_key, &convspec, value_pos);
        }
        return amw_parser_error(parser, end_pos + 1, "Bad character encountered");
    }

    if (chr != AMW_COMMENT) {
        return amw_parser_error(parser, end_pos, "Bad character encountered");
    }

    // read next line
    UwValue status = _amw_read_block_line(parser);
    if (!_amw_end_of_block(&status)) {
        uw_return_if_error(&status);
    }
    return uw_clone(value);
}

static UwResult parse_value(AmwParser* parser, unsigned* nested_value_pos, UwValuePtr convspec_out)
/*
 * Parse value starting from `current_line[block_indent]` .
 *
 * If `nested_value_pos` is provided, the value is _expected_ to be a map key
 * and _must_ end with colon or include a colon if it's a literal strings.
 *
 * On success return parsed value.
 * If `nested_value_pos' is provided, write position of the next char after colon to it
 * and write conversion specifier to `convspec_out` if it's followed by conversion specifier.
 *
 * On error return status and set `parser->result["error"]`.
 */
{
    TRACEPOINT();

    unsigned start_pos = _amw_get_start_position(parser);

    // Analyze first character.
    char32_t chr = uw_char_at(&parser->current_line, start_pos);

    // first, check if value starts with colon that may denote conversion specifier

    if (chr == ':') {
        // this might be conversion specifier
        if (nested_value_pos) {
            // we expect map key, and map keys cannot start with colon
            // because they would look same as conversion specifier
            return amw_parser_error(parser, start_pos, "Map key expected and it cannot start with colon");
        }
        unsigned value_pos;
        UwValue convspec = parse_convspec(parser, start_pos, &value_pos);
        uw_return_if_error(&convspec);

        if (!uw_is_string(&convspec)) {
            // not a conversion specifier
            return parse_literal_string(parser);
        }
        // we have conversion specifier
        if (end_of_line(&parser->current_line, value_pos)) {

            // conversion specifier is followed by LF
            // continue parsing CURRENT block from next line
            UwValue status = _amw_read_block_line(parser);
            if (_amw_end_of_block(&status)) {
                return amw_parser_error(parser, parser->current_indent, "Empty block");
            }
            uw_return_if_error(&status);

            // call parser function
            AmwBlockParserFunc parser_func = get_custom_parser(parser, &convspec);
            return parser_func(parser);

        } else {
            // value is on the same line, parse it as nested block
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
            UwValue number = _amw_parse_number(parser, next_pos, -1, &end_pos, number_terminators);
            return check_value_end(parser, &number, end_pos, nested_value_pos, convspec_out);
        }
        // if followed by space or end of line, that's a list item
        if (isspace_or_eol_at(&parser->current_line, next_pos)) {
            if (nested_value_pos) {
                return amw_parser_error(parser, start_pos, "Map key expected and it cannot be a list");
            }
            // yes, it's a list item
            return parse_list(parser);
        }
        // otherwise, it's a literal string or map
        goto parse_literal_string_or_map;
    }

    // check for quoted string

    if (chr == '"' || chr == '\'') {
        // quoted string
        unsigned start_line = parser->line_number;
        unsigned end_pos;
        UwValue str = parse_quoted_string(parser, start_pos, &end_pos);
        uw_return_if_error(&str);

        unsigned end_line = parser->line_number;
        if (end_line == start_line) {
            // single-line string can be a map key
            return check_value_end(parser, &str, end_pos, nested_value_pos, convspec_out);
        } else if (_amw_comment_or_end_of_line(parser, end_pos)) {
            // multi-line string cannot be a key
            return uw_move(&str);
        } else {
            return amw_parser_error(parser, end_pos, "Bad character after quoted string");
        }
    }

    // check for reserved keywords

    TRACE("trying reserved keywords");
    if (uw_substring_eq(&parser->current_line, start_pos, start_pos + 4, "null")) {
        UwValue null_value = UwNull();
        return check_value_end(parser, &null_value, start_pos + 4, nested_value_pos, convspec_out);
    }
    if (uw_substring_eq(&parser->current_line, start_pos, start_pos + 4, "true")) {
        UwValue true_value = UwBool(true);
        return check_value_end(parser, &true_value, start_pos + 4, nested_value_pos, convspec_out);
    }
    if (uw_substring_eq(&parser->current_line, start_pos, start_pos + 5, "false")) {
        UwValue false_value = UwBool(false);
        return check_value_end(parser, &false_value, start_pos + 5, nested_value_pos, convspec_out);
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
        UwValue number = _amw_parse_number(parser, start_pos, 1, &end_pos, number_terminators);
        return check_value_end(parser, &number, end_pos, nested_value_pos, convspec_out);
    }
    TRACE("not a number, pasring literal string or map");

parse_literal_string_or_map:

    // look for key-value separator
    for (unsigned pos = start_pos;;) {
        unsigned colon_pos;
        if (!uw_strchr(&parser->current_line, ':', pos, &colon_pos)) {
            break;
        }
        UwValue convspec = UwNull();
        unsigned value_pos;
        UwValue kvs = is_kv_separator(parser, colon_pos, &convspec, &value_pos);
        uw_return_if_error(&kvs);

        if (kvs.bool_value) {
            // found key-value separator, get key
            UwValue key = uw_substr(&parser->current_line, start_pos, colon_pos);
            uw_return_if_error(&key);

            // strip trailing spaces
            if (!uw_string_rtrim(&key)) {
                return UwOOM();
            }

            if (nested_value_pos) {
                // key was anticipated, simply return it
                *nested_value_pos = value_pos;
                *convspec_out = uw_move(&convspec);
                return uw_move(&key);
            }

            // parse map
            return parse_map(parser, &key, &convspec, value_pos);
        }
        pos = colon_pos + 1;
    }

    // separator not found

    if (nested_value_pos) {
        // expecting key, but it's a bare literal string
        return amw_parser_error(parser, parser->current_indent, "Not a key");
    }
    return parse_literal_string(parser);
}

static UwResult value_parser_func(AmwParser* parser)
{
    return parse_value(parser, nullptr, nullptr);
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
    uw_return_if_error(&status);

    // parse top-level value
    UwValue result = value_parser_func(parser);
    uw_return_if_error(&result);

    // make sure markup has no more data
    status = _amw_read_block_line(parser);
    if (parser->eof) {
        // all right, no op
    } else {
        uw_return_if_error(&status);
        return amw_parser_error(parser, parser->current_indent, "Extra data after parsed value");
    }
    return uw_move(&result);
}
