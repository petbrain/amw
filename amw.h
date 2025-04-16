#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <uw.h>

#define AMW_MAX_RECURSION_DEPTH  100

#define AMW_COMMENT  '#'

typedef struct {
    /*
     * Parse status.
     */
    unsigned line_number;
    unsigned position;
} AmwStatusData;

#define _amw_status_data_ptr(value)  ((AmwStatusData*) _uw_get_data_ptr((value), UwTypeId_AmwStatus))


extern UwTypeId UwTypeId_AmwStatus;
/*
 * Type ID for AmwStatus value.
 */

/*
 * AMW error codes
 */
extern uint16_t AMW_END_OF_BLOCK;  // for internal use
extern uint16_t AMW_PARSE_ERROR;

typedef struct  {
    _UwValue  markup;
    _UwValue  current_line;
    unsigned  current_indent;  // measured indentation of current line
    unsigned  line_number;
    unsigned  block_indent;    // indent of current block
    unsigned  blocklevel;      // recursion level
    unsigned  max_blocklevel;
    unsigned  json_depth;      // recursion level for JSON
    unsigned  max_json_depth;
    bool      skip_comments;   // initially true to skip leading comments in the block
    bool      eof;
    _UwValue  custom_parsers;
} AmwParser;


AmwParser* amw_create_parser(UwValuePtr markup);
/*
 * Create parser for `markup` which can be either File, StringIO, or any other value
 * that supports line reader interface. See UW library.
 *
 * This function invokes uw_start_read_lines for markup.
 *
 * Return parser on success or nullptr if out of memory.
 */

void amw_delete_parser(AmwParser** parser_ptr);
/*
 * Delete parser. The format of the argument is natural for gnu::cleanup attribute.
 */

typedef UwResult (*AmwBlockParserFunc)(AmwParser* parser);

UwResult amw_set_custom_parser(AmwParser* parser, char* convspec, AmwBlockParserFunc parser_func);
/*
 * Set custom parser function for `convspec`.
 */

UwResult amw_parse(UwValuePtr markup);
/*
 * Parse `markup`.
 *
 * Return parsed value or error.
 */

UwResult amw_parse_json(UwValuePtr markup);
/*
 * Parse `markup` as pure JSON.
 *
 * Return parsed value or error.
 */

UwResult _amw_json_parser_func(AmwParser* parser);
/*
 * JSON parser function for AMW :json: conversion specifier.
 */

UwResult _amw_read_block_line(AmwParser* parser);
/*
 * Read line belonging to a block, until indent is less than `block_indent`.
 * Skip comments with indentation less than `block_indent`.
 *
 * Return success if line is read, AMW_END_OF_BLOCK if there's no more lines
 * in the block, or any other error.
 */

bool _amw_end_of_block(UwValuePtr status);
/*
 * Return true if status is AMW_END_OF_BLOCK
 */

UwResult _amw_read_block(AmwParser* parser);
/*
 * Read lines starting from current_line till the end of block.
 */

unsigned _amw_get_start_position(AmwParser* parser);
/*
 * Return position of the first non-space character in the current block.
 * The block may start inside `current_line` for nested values of list or map.
 */

bool _amw_comment_or_end_of_line(AmwParser* parser, unsigned position);
/*
 * Check if current line ends at position or contains comment.
 */

UwResult _amw_parser_error(AmwParser* parser, char* source_file_name, unsigned source_line_number,
                           unsigned line_number, unsigned char_pos, char* description, ...);
/*
 * Set error in parser->status and return AMW_PARSE_ERROR.
 */

#define amw_parser_error2(parser, line_number, char_pos, description, ...)  \
    _amw_parser_error((parser), __FILE__, __LINE__, (line_number),  \
                      (char_pos), (description) __VA_OPT__(,) __VA_ARGS__)

#define amw_parser_error(parser, char_pos, description, ...)  \
    amw_parser_error2((parser), (parser)->line_number,  \
                      (char_pos), (description) __VA_OPT__(,) __VA_ARGS__)

bool _amw_find_closing_quote(UwValuePtr line, char32_t quote, unsigned start_pos, unsigned* end_pos);
/*
 * Search for closing quotation mark in escaped line.
 * If found, write its position to `end_pos` and return true;
 */

UwResult _amw_unescape_line(AmwParser* parser, UwValuePtr line, unsigned line_number,
                            char32_t quote, unsigned start_pos, unsigned end_pos);
/*
 * Process escaped characters in the `line` from `start_pos` to `end_pos`.
 */

UwResult _amw_parse_number(AmwParser* parser, unsigned start_pos, int sign, unsigned* end_pos, char* allowed_terminators);
/*
 * Parse number, either integer or float.
 * `start_pos` points to the first digit in the `current_line`.
 *
 * Leading zeros in a non-zero decimal numbers are not allowed to avoid ambiguity.
 *
 * Optional single quote (') or underscores can be used as separators.
 *
 * Return numeric value on success. Set `end_pos` to a point where conversion has stopped.
 */

UwResult _amw_parse_json_value(AmwParser* parser, unsigned start_pos, unsigned* end_pos);
/*
 * Parse JSON value starting from `start_pos`.
 * On success write position where parsing stopped to `end_pos`.
 */

#ifdef __cplusplus
}
#endif
