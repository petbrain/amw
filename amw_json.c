#include <amw.h>

UwResult _amw_json_parser_func(AmwParser* parser)
{
    return UwStatus(UW_ERROR_NOT_IMPLEMENTED);
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

    // parse top-level value
    UwValue result = _amw_json_parser_func(parser);
    uw_return_if_error(&result);

    // make sure markup has no more data
    status = _amw_read_block_line(parser);
    if (parser->eof) {
        // all right, no op
    } else {
        return amw_parser_error(parser, parser->current_indent, "Extra data after parsed value");
    }
    return uw_move(&result);
}
