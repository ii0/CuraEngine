// Copyright (c) 2023 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher

#include "InsetOrderOptimizer.h"

#include <tuple>

#include <range/v3/algorithm/max.hpp>
#include <range/v3/algorithm/sort.hpp>
#include <range/v3/range/conversion.hpp>
#include <range/v3/view/addressof.hpp>
#include <range/v3/view/any_view.hpp>
#include <range/v3/view/drop.hpp>
#include <range/v3/view/drop_last.hpp>
#include <range/v3/view/filter.hpp>
#include <range/v3/view/join.hpp>
#include <range/v3/view/remove_if.hpp>
#include <range/v3/view/reverse.hpp>
#include <range/v3/view/take_exactly.hpp>
#include <range/v3/view/transform.hpp>

#include "ExtruderTrain.h"
#include "FffGcodeWriter.h"
#include "LayerPlan.h"
#include "utils/views/convert.h"
#include "utils/views/dfs.h"

namespace rg = ranges;
namespace rv = ranges::views;

namespace cura
{

InsetOrderOptimizer::InsetOrderOptimizer(
    const FffGcodeWriter& gcode_writer,
    const SliceDataStorage& storage,
    LayerPlan& gcode_layer,
    const Settings& settings,
    const int extruder_nr,
    const GCodePathConfig& inset_0_non_bridge_config,
    const GCodePathConfig& inset_X_non_bridge_config,
    const GCodePathConfig& inset_0_bridge_config,
    const GCodePathConfig& inset_X_bridge_config,
    const bool retract_before_outer_wall,
    const coord_t wall_0_wipe_dist,
    const coord_t wall_x_wipe_dist,
    const size_t wall_0_extruder_nr,
    const size_t wall_x_extruder_nr,
    const ZSeamConfig& z_seam_config,
    const std::vector<VariableWidthLines>& paths)
    : gcode_writer_(gcode_writer)
    , storage_(storage)
    , gcode_layer_(gcode_layer)
    , settings_(settings)
    , extruder_nr_(extruder_nr)
    , inset_0_non_bridge_config_(inset_0_non_bridge_config)
    , inset_X_non_bridge_config_(inset_X_non_bridge_config)
    , inset_0_bridge_config_(inset_0_bridge_config)
    , inset_X_bridge_config_(inset_X_bridge_config)
    , retract_before_outer_wall_(retract_before_outer_wall)
    , wall_0_wipe_dist_(wall_0_wipe_dist)
    , wall_x_wipe_dist_(wall_x_wipe_dist)
    , wall_0_extruder_nr_(wall_0_extruder_nr)
    , wall_x_extruder_nr_(wall_x_extruder_nr)
    , z_seam_config_(z_seam_config)
    , paths_(paths)
    , layer_nr_(gcode_layer.getLayerNr())
{
}

bool InsetOrderOptimizer::addToLayer()
{
    // Settings & configs:
    const auto pack_by_inset = ! settings_.get<bool>("optimize_wall_printing_order");
    const auto inset_direction = settings_.get<InsetDirection>("inset_direction");
    const auto alternate_walls = settings_.get<bool>("material_alternate_walls");

    const bool outer_to_inner = inset_direction == InsetDirection::OUTSIDE_IN;
    const bool use_one_extruder = wall_0_extruder_nr_ == wall_x_extruder_nr_;
    const bool current_extruder_is_wall_x = wall_x_extruder_nr_ == extruder_nr_;

    const bool reverse = shouldReversePath(use_one_extruder, current_extruder_is_wall_x, outer_to_inner);
    auto walls_to_be_added = getWallsToBeAdded(reverse, use_one_extruder);

    const auto order = pack_by_inset ? getInsetOrder(walls_to_be_added, outer_to_inner) : getRegionOrder(walls_to_be_added, outer_to_inner);

    constexpr Ratio flow = 1.0_r;

    bool added_something = false;

    constexpr bool detect_loops = false;
    constexpr Polygons* combing_boundary = nullptr;
    const auto group_outer_walls = settings_.get<bool>("group_outer_walls");
    // When we alternate walls, also alternate the direction at which the first wall starts in.
    // On even layers we start with normal direction, on odd layers with inverted direction.
    PathOrderOptimizer<const ExtrusionLine*>
        order_optimizer(gcode_layer_.getLastPlannedPositionOrStartingPosition(), z_seam_config_, detect_loops, combing_boundary, reverse, order, group_outer_walls);

    for (const auto& line : walls_to_be_added)
    {
        if (line.is_closed_)
        {
            order_optimizer.addPolygon(&line);
        }
        else
        {
            order_optimizer.addPolyline(&line);
        }
    }

    order_optimizer.optimize();

    for (const PathOrdering<const ExtrusionLine*>& path : order_optimizer.paths_)
    {
        if (path.vertices_->empty())
            continue;

        const bool is_outer_wall = path.vertices_->inset_idx_ == 0; // or thin wall 'gap filler'
        const bool is_gap_filler = path.vertices_->is_odd_;
        const GCodePathConfig& non_bridge_config = is_outer_wall ? inset_0_non_bridge_config_ : inset_X_non_bridge_config_;
        const GCodePathConfig& bridge_config = is_outer_wall ? inset_0_bridge_config_ : inset_X_bridge_config_;
        const coord_t wipe_dist = is_outer_wall && ! is_gap_filler ? wall_0_wipe_dist_ : wall_x_wipe_dist_;
        const bool retract_before = is_outer_wall ? retract_before_outer_wall_ : false;

        const bool revert_inset = alternate_walls && (path.vertices_->inset_idx_ % 2);
        const bool revert_layer = alternate_walls && (layer_nr_ % 2);
        const bool backwards = path.backwards_ != (revert_inset != revert_layer);
        const size_t start_index = (backwards != path.backwards_) ? path.vertices_->size() - (path.start_vertex_ + 1) : path.start_vertex_;
        const bool linked_path = ! path.is_closed_;

        gcode_layer_.setIsInside(true); // Going to print walls, which are always inside.
        gcode_layer_.addWall(*path.vertices_, start_index, settings_, non_bridge_config, bridge_config, wipe_dist, flow, retract_before, path.is_closed_, backwards, linked_path);
        added_something = true;
    }
    return added_something;
}

InsetOrderOptimizer::value_type InsetOrderOptimizer::getRegionOrder(const std::vector<ExtrusionLine>& input, const bool outer_to_inner)
{
    if (input.empty()) // Early out
    {
        return {};
    }

    // Cache the polygons and get the signed area of each extrusion line and store them mapped against the pointers for those lines
    struct LineLoc
    {
        const ExtrusionLine* line;
        Polygon poly;
        coord_t area;
        bool is_outer;
    };

    size_t min_inset_idx = std::numeric_limits<size_t>::max();
    for (const auto& line : input)
    {
        min_inset_idx = std::min(min_inset_idx, line.inset_idx_);
    }

    auto locator_view = input | rv::addressof
                      | rv::transform(
                            [&min_inset_idx](const ExtrusionLine* extrusion_line)
                            {
                                const auto poly = extrusion_line->toPolygon();
                                AABB aabb;
                                aabb.include(poly);
                                return LineLoc{
                                    .line = extrusion_line,
                                    .poly = poly,
                                    .area = aabb.area(),
                                    .is_outer = extrusion_line->inset_idx_ == min_inset_idx,
                                };
                            })
                      | rg::to_vector;

    // Sort polygons by increasing area, we are building the graph from the leaves (smallest area) upwards.
    rg::sort(
        locator_view,
        [](const auto& lhs, const auto& rhs)
        {
            return lhs < rhs;
        },
        &LineLoc::area);

    // Create a bi-direction directed acyclic graph (Tree). Where polygon B is a child of A if B is inside A. The root of the graph is
    // the polygon that contains all other polygons. The leaves are polygons that contain no polygons.
    // We need a bi-directional graph as we are performing a dfs from the root down and from each of the hole (which are leaves in the graph) up the tree
    std::unordered_multimap<const LineLoc*, const LineLoc*> graph;
    std::unordered_set<LineLoc*> roots;
    for (const auto& locator : locator_view | rv::addressof)
    {
        std::vector<LineLoc*> erase;
        for (const auto& root : roots)
        {
            if (locator->poly.inside(root->poly[0], false))
            {
                // The root polygon is inside the location polygon. It is no longer a root in the graph we are building.
                // Add this relationship (locator <-> root) to the graph, and remove root from roots.
                graph.emplace(locator, root);
                graph.emplace(root, locator);
                erase.emplace_back(root);
            }
        }
        for (const auto& node : erase)
        {
            roots.erase(node);
        }
        // We are adding to the graph from smallest area -> largest area. This means locator will always be the largest polygon in the graph so far.
        // No polygon in the graph is big enough to contain locator, so it must be a root.
        roots.emplace(locator);
    }

    std::unordered_multimap<const ExtrusionLine*, const ExtrusionLine*> order;

    // find for each line the closest outer line
    std::unordered_map<const LineLoc*, unsigned int> min_depth;
    std::unordered_map<const LineLoc*, const LineLoc*> min_node;
    for (const LineLoc* root_node : locator_view
                                        | rv::filter(
                                            [](const auto& locator)
                                            {
                                                return locator.is_outer;
                                            })
                                        | rv::addressof)
    {
        const std::function<void(const LineLoc*, const unsigned int)> update_nodes = [&root_node, &min_depth, &min_node](const auto& current_node, auto depth)
        {
            if (min_depth.find(current_node) == min_depth.end() || depth < min_depth[current_node])
            {
                min_depth[current_node] = depth;
                min_node[current_node] = root_node;
            }
        };

        actions::dfs_depth_state(root_node, graph, update_nodes);
    }

    // perform a dfs from the root and all hole roots $r$ and set the order constraints for each polyline for which
    // the depth is closest to root $r$
    for (const LineLoc* root_node : locator_view
                                        | rv::filter(
                                            [](const auto& locator)
                                            {
                                                return locator.is_outer;
                                            })
                                        | rv::addressof)
    {
        const std::function<void(const LineLoc*, const LineLoc*)> set_order_constraints
            = [&order, &min_node, &root_node, &outer_to_inner](const auto& current_node, const auto& parent_node)
        {
            // if parent root is n
            if (min_node[current_node] == root_node && parent_node != nullptr)
            {
                // flip the key values if we want to print from inner to outer walls
                if (outer_to_inner)
                {
                    order.insert(std::make_pair(parent_node->line, current_node->line));
                }
                else
                {
                    order.insert(std::make_pair(current_node->line, parent_node->line));
                }
            }
        };

        actions::dfs_parent_state(root_node, graph, set_order_constraints);
    }

    return order;
}

InsetOrderOptimizer::value_type InsetOrderOptimizer::getInsetOrder(const auto& input, const bool outer_to_inner)
{
    value_type order;

    std::vector<std::vector<const ExtrusionLine*>> walls_by_inset;
    std::vector<std::vector<const ExtrusionLine*>> fillers_by_inset;

    for (const auto& line : input)
    {
        if (line.is_odd_)
        {
            if (line.inset_idx_ >= fillers_by_inset.size())
            {
                fillers_by_inset.resize(line.inset_idx_ + 1);
            }
            fillers_by_inset[line.inset_idx_].emplace_back(&line);
        }
        else
        {
            if (line.inset_idx_ >= walls_by_inset.size())
            {
                walls_by_inset.resize(line.inset_idx_ + 1);
            }
            walls_by_inset[line.inset_idx_].emplace_back(&line);
        }
    }
    for (size_t inset_idx = 0; inset_idx + 1 < walls_by_inset.size(); inset_idx++)
    {
        for (const ExtrusionLine* line : walls_by_inset[inset_idx])
        {
            for (const ExtrusionLine* inner_line : walls_by_inset[inset_idx + 1])
            {
                const ExtrusionLine* before = inner_line;
                const ExtrusionLine* after = line;
                if (outer_to_inner)
                {
                    std::swap(before, after);
                }
                order.emplace(before, after);
            }
        }
    }
    for (size_t inset_idx = 1; inset_idx < fillers_by_inset.size(); inset_idx++)
    {
        for (const ExtrusionLine* line : fillers_by_inset[inset_idx])
        {
            if (inset_idx - 1 >= walls_by_inset.size())
                continue;
            for (const ExtrusionLine* enclosing_wall : walls_by_inset[inset_idx - 1])
            {
                order.emplace(enclosing_wall, line);
            }
        }
    }

    return order;
}

constexpr bool InsetOrderOptimizer::shouldReversePath(const bool use_one_extruder, const bool current_extruder_is_wall_x, const bool outer_to_inner)
{
    if (use_one_extruder && current_extruder_is_wall_x)
    {
        return ! outer_to_inner;
    }
    return current_extruder_is_wall_x;
}

std::vector<ExtrusionLine> InsetOrderOptimizer::getWallsToBeAdded(const bool reverse, const bool use_one_extruder)
{
    if (paths_.empty())
    {
        return {};
    }
    rg::any_view<VariableWidthLines> view;
    if (reverse)
    {
        if (use_one_extruder)
        {
            view = paths_ | rv::reverse;
        }
        else
        {
            view = paths_ | rv::reverse | rv::drop_last(1);
        }
    }
    else
    {
        if (use_one_extruder)
        {
            view = paths_ | rv::all;
        }
        else
        {
            view = paths_ | rv::take_exactly(1);
        }
    }
    return view | rv::join | rv::remove_if(rg::empty) | rg::to_vector;
}
} // namespace cura
