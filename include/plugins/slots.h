// Copyright (c) 2023 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher

#ifndef CURAENGINE_INCLUDE_PLUGINS_SLOTS_H
#define CURAENGINE_INCLUDE_PLUGINS_SLOTS_H

#include <memory>
#include <unordered_map>
#include <variant>

#include "plugins/pluginproxy.h"
#include "plugins/validator.h"
#include "plugins/types.h"

namespace cura::plugins
{

using simplify_plugin = PluginProxy<SlotID::SIMPLIFY, Validator<">=1.0.0 <2.0.0 || >3.2.1", "">, details::simplify_converter_fn<proto::Simplify_args, proto::Simplify_ret>>;
using postprocess_plugin = PluginProxy<SlotID::POSTPROCESS, Validator<">=1.0.0 <2.0.0 || >3.2.1", "">, details::postprocess_converter_fn<proto::Postprocess_args, proto::Postprocess_ret>>;


using plugins_t = std::variant<simplify_plugin, postprocess_plugin>;


class Slots
{
    constexpr Slots() noexcept = default;
    std::unordered_map<SlotID, plugins_t> slots_{};

public:
    Slots(const Slots&) = delete;
    Slots(Slots&&) = delete;

    static Slots& instance() noexcept
    {
        static Slots instance{};
        return instance;
    }



//    template<class Slot>
//    constexpr void registerSlot(Slot&& slot)
//    {
//        slots_.emplace(slot.slot_id, std::forward<Slot>(slot));
//    }
//
//
//    simplify_slot getSlot()
//    {
//        return std::get<simplify_slot>(slots_.at(SlotID::SIMPLIFY));
//    }

};


} // namespace cura::plugins


#endif // CURAENGINE_INCLUDE_PLUGINS_SLOTS_H