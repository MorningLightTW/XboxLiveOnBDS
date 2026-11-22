#pragma once

#include <string_view>

#include "ll/api/io/Logger.h"

#include "xbox_live_on_bds/net/Http.h"
#include "xbox_live_on_bds/util/Redact.h"

namespace xbox_live_on_bds::net {

/// Logs a failed request as "<what> (<cause>)": warn when trying again can help, else error. The details go to a
/// debug line, under `step` (the request, for a reader of the debug log), with IPs, ids and tokens replaced. Apart
/// from Http.h so that the offline tests need no LeviLamina.
inline void
logFailure(ll::io::Logger& logger, std::string_view what, HttpResponse const& res, std::string_view step = {}) {
    logger.debug("{}: {}", step.empty() ? what : step, util::Redactor::instance().text(res.describe()));
    if (res.temporary()) logger.warn("{} ({})", what, res.plainCause());
    else logger.error("{} ({})", what, res.plainCause());
}

} // namespace xbox_live_on_bds::net
