#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "xbox_live_on_bds/auth/EcKey.h"

namespace xbox_live_on_bds::auth {

/// The key's public half as the JWK Xbox Live expects in "ProofKey".
nlohmann::json proofKey(EcKey const& key);

/// The "Signature" header (signing policy version 1) for a POST to `path` with `body` and no Authorization header.
std::string signRequest(EcKey const& key, std::string_view path, std::string_view body);

} // namespace xbox_live_on_bds::auth
