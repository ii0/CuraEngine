// Copyright (c) 2023 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher

#ifndef CURAENGINE_INCLUDE_PLUGINS_METADATA_H
#define CURAENGINE_INCLUDE_PLUGINS_METADATA_H

#include <map>
#include <string>

#include <grpcpp/support/string_ref.h>
#include <string_view>

#include "plugins/types.h"

namespace cura::plugins
{
struct plugin_metadata
{
    std::string_view name;
    std::string_view version;
    std::string_view peer;
    std::string_view slot_version;

    explicit plugin_metadata(const grpc::ClientContext& client_context)
    {
        const auto& metadata = client_context.GetServerInitialMetadata();
        if (auto it = metadata.find("cura-slot-version"); it != metadata.end())
        {
            slot_version = std::string{ it->second.data(), it->second.size() };
        }
        else
        {
            spdlog::error("'cura-slot-version' RPC metadata not set");
            throw std::runtime_error("'cura-slot-version' RPC metadata not set");
        }
        if (auto it = metadata.find("cura-plugin-name"); it != metadata.end())
        {
            name = std::string{ it->second.data(), it->second.size() };
        }
        else
        {
            spdlog::warn("'cura-plugin-name' RPC metadata not set");
        }
        if (auto it = metadata.find("cura-plugin-version"); it != metadata.end())
        {
            version = std::string{ it->second.data(), it->second.size() };
        }
        else
        {
            spdlog::warn("'cura-plugin-version' RPC metadata not set");
        }
        peer = client_context.peer();
    }
};

struct slot_metadata
{
    plugins::v1::SlotID slot_id;
    std::string_view version_range;
};

} // namespace cura::plugins

#endif // CURAENGINE_INCLUDE_PLUGINS_METADATA_H