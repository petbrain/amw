#include <amw.h>

static char number_terminators[] = { AMW_COMMENT, ':', ',', '}', ']', 0 };


static UwResult skip_spaces(AmwParser* parser, unsigned* pos, unsigned source_line)
/*
 * Skip spaces and comments before structural element.
 *
 * On success return Unsigned value containing first non-space character.
 *
 * On error `source_line` is set in returned status.
 */
{
    for (;;) {
        UwValuePtr current_line = &parser->current_line;

        *pos = uw_string_skip_spaces(current_line, *pos);

        // end of line?
        if (uw_string_index_valid(current_line, *pos)) {
            // no, return character if not a comment
            char32_t chr = uw_char_at(current_line, *pos);
            if (chr != '#') {
                return UwUnsigned(chr);
            }
        }
        // read next line
        UwValue status = _amw_read_block_line(parser);
        if (_amw_end_of_block(&status)) {
            UwValue error = amw_parser_error(parser, parser->current_indent, "Unexpected end of block");
            if (error.status_code == AMW_PARSE_ERROR) {
                _uw_set_status_location(&error, __FILE__, source_line);
            }
            return uw_move(&error);
        }
        *pos = parser->current_indent;
    }
}

static UwResult parse_number(AmwParser* parser, unsigned start_pos, unsigned* end_pos)
/*
 * `start_pos` points to the sign or first digit
 */
{
    int sign = 1;
    char32_t chr = uw_char_at(&parser->current_line, start_pos);
    if (chr == '+') {
        // no op
    } else if (chr == '-') {
        sign = -1;
    }
    return _amw_parse_number(parser, start_pos, sign, end_pos, number_terminators);
}

static UwResult parse_string(AmwParser* parser, unsigned start_pos, unsigned* end_pos)
/*
 * `start_pos` points to the opening double quotation mark (")
 */
{
    if (_amw_find_closing_quote(&parser->current_line, '"', start_pos + 1, end_pos)) {
        (*end_pos)++;
        return _amw_unescape_line(parser, &parser->current_line,
                                  parser->line_number, '"', start_pos + 1, nullptr);
    }
    return amw_parser_error(parser, parser->current_indent, "String has no closing quote");
}

static UwResult parse_array(AmwParser* parser, unsigned start_pos, unsigned* end_pos)
/*
 * `start_pos` points to the next character after opening square bracket
 */
{
    parser->json_depth++;

    UwValue result = UwArray();
    uw_return_if_error(&result);

    UwValue chr = skip_spaces(parser, &start_pos, __LINE__);
    uw_return_if_error(&chr);

    if (chr.unsigned_value == ']') {
        // empty array
        *end_pos = start_pos + 1;
        parser->json_depth--;
        return uw_move(&result);
    }

    // parse first item
    UwValue first_item = _amw_parse_json_value(parser, start_pos, &start_pos);
    uw_return_if_error(&first_item);

    if (!uw_array_append(&result, &first_item)) {
        return UwOOM();
    }

    // parse subsequent items
    for (;;) {{
        chr = skip_spaces(parser, &start_pos, __LINE__);
        uw_return_if_error(&chr);

        if (chr.unsigned_value == ']') {
            // done
            *end_pos = start_pos + 1;
            parser->json_depth--;
            return uw_move(&result);
        }
        if (chr.unsigned_value != ',') {
            return amw_parser_error(parser, parser->current_indent, "Array items must be separated with comma");
        }
        UwValue item = _amw_parse_json_value(parser, start_pos + 1, &start_pos);
        uw_return_if_error(&item);

        if (!uw_array_append(&result, &item)) {
            return UwOOM();
        }
    }}
}

static UwResult parse_object_member(AmwParser* parser, unsigned* pos, UwValuePtr result)
/*
 * Parse key:value pair starting from `pos` and update `result`.
 *
 * Update `pos` on exit.
 */
{
    UwValue key = parse_string(parser, *pos, pos);
    uw_return_if_error(&key);

    UwValue chr = skip_spaces(parser, pos, __LINE__);
    uw_return_if_error(&chr);

    if (chr.unsigned_value != ':') {
        return amw_parser_error(parser, parser->current_indent, "Values must be separated from keys with colon");
    }

    (*pos)++;

    UwValue value = _amw_parse_json_value(parser, *pos, pos);
    uw_return_if_error(&value);

    if (!uw_map_update(result, &key, &value)) {
        return UwOOM();
    }
    return UwOK();
}

static UwResult parse_object(AmwParser* parser, unsigned start_pos, unsigned* end_pos)
/*
 * `start_pos` points to the next character after opening curly bracket
 */
{
    parser->json_depth++;

    UwValue result = UwMap();
    uw_return_if_error(&result);

    UwValue chr = skip_spaces(parser, &start_pos, __LINE__);
    uw_return_if_error(&chr);

    if (chr.unsigned_value == '}') {
        // empty object
        *end_pos = start_pos + 1;
        parser->json_depth--;
        return uw_move(&result);
    }

    // parse first member
    UwValue status = parse_object_member(parser, &start_pos, &result);
    uw_return_if_error(&status);

    // parse subsequent members
    for (;;) {{
        chr = skip_spaces(parser, &start_pos, __LINE__);
        uw_return_if_error(&chr);

        if (chr.unsigned_value == '}') {
            // done
            *end_pos = start_pos + 1;
            parser->json_depth--;
            return uw_move(&result);
        }
        if (chr.unsigned_value != ',') {
            return amw_parser_error(parser, parser->current_indent, "Object members must be separated with comma");
        }
        start_pos++;
        chr = skip_spaces(parser, &start_pos, __LINE__);
        uw_return_if_error(&chr);

        UwValue status = parse_object_member(parser, &start_pos, &result);
        uw_return_if_error(&status);
    }}
}

UwResult _amw_parse_json_value(AmwParser* parser, unsigned start_pos, unsigned* end_pos)
{
    if (parser->json_depth >= parser->max_json_depth) {
        return amw_parser_error(parser, parser->current_indent, "Maximum recursion depth exceeded");
    }

    UwValue first_char = skip_spaces(parser, &start_pos, __LINE__);
    uw_return_if_error(&first_char);

    char32_t chr = first_char.unsigned_value;

    if (chr == '[') {
        return parse_array(parser, start_pos + 1, end_pos);
    }
    if (chr == '{') {
        return parse_object(parser, start_pos + 1, end_pos);
    }
    if (chr == '"') {
        return parse_string(parser, start_pos, end_pos);
    }
    if (chr == '+' || chr == '-' || uw_isdigit(chr)) {
        return parse_number(parser, start_pos, end_pos);
    }
    if (uw_substring_eq(&parser->current_line, start_pos, start_pos + 4, "null")) {
        *end_pos = start_pos + 4;
        return UwNull();
    }
    if (uw_substring_eq(&parser->current_line, start_pos, start_pos + 4, "true")) {
        *end_pos = start_pos + 4;
        return UwBool(true);
    }
    if (uw_substring_eq(&parser->current_line, start_pos, start_pos + 5, "false")) {
        *end_pos = start_pos + 5;
        return UwBool(false);
    }
    return amw_parser_error(parser, start_pos, "Unexpected character");
}

UwResult _amw_json_parser_func(AmwParser* parser)
{
    unsigned end_pos;
    UwValue result = _amw_parse_json_value(parser, _amw_get_start_position(parser), &end_pos);
    uw_return_if_error(&result);

    // check trailing characters

    static char garbage[] = "Gabage after JSON value";

    if (_amw_comment_or_end_of_line(parser, end_pos)) {

        // make sure current block has no more data
        UwValue status = _amw_read_block_line(parser);
        if (!_amw_end_of_block(&status)) {
            return amw_parser_error(parser, parser->current_indent, garbage);
        }
    } else {
        return amw_parser_error(parser, parser->current_indent, garbage);
    }
    return uw_move(&result);
}

UwResult amw_parse_json(UwValuePtr markup)
{
    [[ gnu::cleanup(amw_delete_parser) ]] AmwParser* parser = amw_create_parser(markup);
    if (!parser) {
        return UwOOM();
    }
    // read first line to prepare for parsing and to detect EOF
    UwValue status = _amw_read_block_line(parser);
    uw_return_if_error(&status);

    // parse root value
    unsigned end_pos;
    UwValue result = _amw_parse_json_value(parser, 0, &end_pos);
    uw_return_if_error(&result);

    // make sure markup has no more data

    static char extra_data[] = "Extra data after parsed value";

    if (!_amw_comment_or_end_of_line(parser, end_pos)) {
        return amw_parser_error(parser, parser->current_indent, extra_data);
    }
    // make sure current block has no more data
    status = _amw_read_block_line(parser);
    if (parser->eof) {
        // all right, no op
    } else {
        uw_return_if_error(&status);
        return amw_parser_error(parser, parser->current_indent, extra_data);
    }
    return uw_move(&result);
}
