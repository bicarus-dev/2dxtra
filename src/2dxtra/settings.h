#pragma once

#include "database.h"

namespace iidxtra::settings
{
    auto init(database::db*) -> void;
    auto save() -> bool;
    auto reset(bool include_event_mode = true) -> void;
}