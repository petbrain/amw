#include <amw.h>

UwTypeId UwTypeId_AmwStatus = 0;

uint16_t AMW_END_OF_BLOCK = 0;
uint16_t AMW_PARSE_ERROR = 0;

static UwResult amw_status_create(UwTypeId type_id, void* ctor_args)
{
    // call super method

    UwValue status = uw_ancestor_of(UwTypeId_AmwStatus)->create(type_id, ctor_args);
    // the super method returns UW_SUCCESS by default
    uw_return_if_error(&status);

    // the base Status constructor may not allocate struct_data
    // do this by setting status description

    _uw_set_status_desc(&status, "");
    if (status.struct_data == nullptr) {
        return UwOOM();
    }
    return uw_move(&status);
}

static UwResult amw_status_init(UwValuePtr self, void* ctor_args)
{
    AmwStatusData* data = _amw_status_data_ptr(self);
    data->line_number = 0;
    data->position = 0;
    return UwOK();
}

static void amw_status_hash(UwValuePtr self, UwHashContext* ctx)
{
    AmwStatusData* data = _amw_status_data_ptr(self);

    _uw_hash_uint64(ctx, self->type_id);
    _uw_hash_uint64(ctx, data->line_number);
    _uw_hash_uint64(ctx, data->position);

    // call super method

    uw_ancestor_of(UwTypeId_AmwStatus)->hash(self, ctx);
}

static UwResult amw_status_to_string(UwValuePtr self)
{
    AmwStatusData* data = _amw_status_data_ptr(self);

    char location[48];
    snprintf(location, sizeof(location), "Line %u, position %u: ",
             data->line_number, data->position);

    UwValue result = uw_create_string(location);
    uw_return_if_error(&result);

    UwValue status_str = uw_ancestor_of(UwTypeId_AmwStatus)->to_string(self);
    uw_return_if_error(&status_str);

    if (!uw_string_append(&result, &status_str)) {
        return UwOOM();
    }

    return uw_move(&result);
}

static UwType amw_status_type;

[[ gnu::constructor ]]
static void init_amw_status()
{
    UwTypeId_AmwStatus = uw_subtype(&amw_status_type, "AmwStatus", UwTypeId_Status, AmwStatusData);
    amw_status_type.create    = amw_status_create;
    amw_status_type.init      = amw_status_init;
    amw_status_type.hash      = amw_status_hash;
    amw_status_type.to_string = amw_status_to_string;

    // init status codes
    AMW_END_OF_BLOCK = uw_define_status("END_OF_BLOCK");
    AMW_PARSE_ERROR  = uw_define_status("PARSE_ERROR");
}
