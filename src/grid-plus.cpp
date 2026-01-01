#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/core.hpp>
#include <wayfire/view.hpp>
#include <wayfire/workarea.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/render-manager.hpp>
#include <cmath>
#include <linux/input-event-codes.h>
#include "wayfire/plugin.hpp"
#include "wayfire/signal-definitions.hpp"
#include <wayfire/plugins/common/geometry-animation.hpp>
#include <wayfire/plugins/common/preview-indication.hpp>
#include "wayfire/plugins/grid.hpp"
#include "wayfire/plugins/crossfade.hpp"
#include <wayfire/window-manager.hpp>

#include <wayfire/plugins/wobbly/wobbly-signal.hpp>
#include <wayfire/view-transform.hpp>
#include "wayfire/plugins/ipc/ipc-activator.hpp"
#include "wayfire/signal-provider.hpp"

const std::string grid_view_id = "grid-view";

class wf_grid_slot_data : public wf::custom_data_t
{
  public:
    int slot;
};

nonstd::observer_ptr<wf::grid::grid_animation_t> ensure_grid_view(wayfire_toplevel_view view)
{
    if (!view->has_data<wf::grid::grid_animation_t>())
    {
        wf::option_wrapper_t<std::string> animation_type{"grid/type"};
        wf::option_wrapper_t<wf::animation_description_t> duration{"grid/duration"};

        wf::grid::grid_animation_t::type_t type = wf::grid::grid_animation_t::NONE;
        if (animation_type.value() == "crossfade")
        {
            type = wf::grid::grid_animation_t::CROSSFADE;
        } else if (animation_type.value() == "wobbly")
        {
            type = wf::grid::grid_animation_t::WOBBLY;
        }

        view->store_data(
            std::make_unique<wf::grid::grid_animation_t>(view, type, duration));
    }

    return view->get_data<wf::grid::grid_animation_t>();
}

class wayfire_grid_plus : public wf::plugin_interface_t, public wf::per_output_tracker_mixin_t<>
{
    std::vector<std::string> slots = {"unused", "bl", "b", "br", "l", "c", "r", "tl", "t", "tr"};
    wf::ipc_activator_t bindings[10];
    wf::ipc_activator_t restore{"grid/restore"};
    wf::option_wrapper_t<int> snap_threshold{"move/snap_threshold"};
    wf::option_wrapper_t<int> quarter_snap_threshold{"move/quarter_snap_threshold"};
    wf::option_wrapper_t<wf::config::compound_list_t<
            std::string, std::string>> maximize_regions{"grid-plus/region_lists"};
    struct
    {
        std::shared_ptr<wf::preview_indication_t> preview;
        wf::grid::slot_t slot_id = wf::grid::SLOT_NONE;
    } slot;

    wf::plugin_activation_data_t grab_interface{
        .name = "grid",
        .capabilities = wf::CAPABILITY_MANAGE_DESKTOP,
    };

    wf::ipc_activator_t::handler_t handle_restore = [=] (wf::output_t *wo, wayfire_view view)
    {
        if (!wo->can_activate_plugin(&grab_interface))
        {
            return false;
        }

        if (auto toplevel = toplevel_cast(view))
        {
            wf::get_core().default_wm->tile_request(toplevel, 0);
            return true;
        }

        return false;
    };

    static std::vector<wf::geometry_t> get_maximize_regions_from_string(
        std::string maximize_regions)
    {
        size_t last = 0;
        size_t next = 0;
        const char *current_region;
        std::vector<wf::geometry_t> region_list;
        std::string curr;
        wf::geometry_t region;
        while (last < maximize_regions.length())
        {
            next = maximize_regions.find(",", last);
            if (next == std::string::npos)
            {
                next = maximize_regions.length();
            }
            curr = maximize_regions.substr(last, next - last);
            current_region = curr.c_str();
            if (sscanf(current_region, "%dx%d+%d+%d", &region.width,
                &region.height, &region.x, &region.y) < 4)
            {
                LOGE("Bad maximize region in config: ", curr);
                return region_list;
            }
            region_list.push_back(region);
            last = next + 1;
        }

        return region_list;
    }

    wf::geometry_t get_maximize_region(wf::output_t *output, wf::geometry_t view_geometry)
    {
       int new_area = 0;
       int last_area = 0;
       auto current_workarea = output->workarea->get_workarea();
       wf::geometry_t best_region = current_workarea;
       wf::geometry_t intersection;
       for (const auto& [_, _output, _regions] : maximize_regions.value())
       {
           if (output->to_string() != (std::string) _output)
               continue;

           std::vector<wf::geometry_t> region_list = get_maximize_regions_from_string(_regions);
           for (auto r : region_list)
           {
               intersection = wf::geometry_intersection(view_geometry, r);
               new_area = intersection.width * intersection.height;
               if (new_area > last_area)
               {
                   best_region = wf::geometry_intersection(current_workarea, r);
                   last_area = new_area;
               }
           }
       }
       return best_region;
    }

    /*
     * 7 8 9
     * 4 5 6
     * 1 2 3
     * */
    inline wf::geometry_t _get_slot_dimensions(wf::output_t *output, wf::geometry_t view_g, int n)
    {
        auto area = get_maximize_region(output, view_g);
        int w2    = area.width / 2;
        int h2    = area.height / 2;

        if (n % 3 == 1)
        {
            area.width = w2;
        }

        if (n % 3 == 0)
        {
            area.width = w2, area.x += w2;
        }

        if (n >= 7)
        {
            area.height = h2;
        } else if (n <= 3)
        {
            area.height = h2, area.y += h2;
        }

        return area;
    }

    /* Calculate the slot to which the view would be snapped if the input
     * is released at output-local coordinates (x, y) */
    inline wf::grid::slot_t _calc_slot(wf::output_t *output, wf::point_t point, int snap_threshold,
        int quarter_snap_threshold)
    {
        wf::geometry_t view_g = { point.x, point.y, 1, 1 };
        auto g = get_maximize_region(output, view_g);

        int threshold = snap_threshold;

        bool is_left   = point.x - g.x <= threshold;
        bool is_right  = g.x + g.width - point.x <= threshold;
        bool is_top    = point.y - g.y < threshold;
        bool is_bottom = g.x + g.height - point.y < threshold;

        bool is_far_left   = point.x - g.x <= quarter_snap_threshold;
        bool is_far_right  = g.x + g.width - point.x <= quarter_snap_threshold;
        bool is_far_top    = point.y - g.y < quarter_snap_threshold;
        bool is_far_bottom = g.x + g.height - point.y < quarter_snap_threshold;

        wf::grid::slot_t slot = wf::grid::SLOT_NONE;
        if ((is_left && is_far_top) || (is_far_left && is_top))
        {
            slot = wf::grid::SLOT_TL;
        } else if ((is_right && is_far_top) || (is_far_right && is_top))
        {
            slot = wf::grid::SLOT_TR;
        } else if ((is_right && is_far_bottom) || (is_far_right && is_bottom))
        {
            slot = wf::grid::SLOT_BR;
        } else if ((is_left && is_far_bottom) || (is_far_left && is_bottom))
        {
            slot = wf::grid::SLOT_BL;
        } else if (is_right)
        {
            slot = wf::grid::SLOT_RIGHT;
        } else if (is_left)
        {
            slot = wf::grid::SLOT_LEFT;
        } else if (is_top)
        {
            // Maximize when dragging to the top
            slot = wf::grid::SLOT_CENTER;
        } else if (is_bottom)
        {
            slot = wf::grid::SLOT_BOTTOM;
        }

        return slot;
    }

  public:
    void init() override
    {
        init_output_tracking();
        restore.set_handler(handle_restore);
        for (int i = 1; i < 10; i++)
        {
            bindings[i].load_from_xml_option("grid/slot_" + slots[i]);
            bindings[i].set_handler([=] (wf::output_t *wo, wayfire_view view)
            {
                if (!wo->can_activate_plugin(&grab_interface))
                {
                    return false;
                }

                if (auto toplevel = toplevel_cast(view))
                {
                    handle_slot(toplevel, i);
                    return true;
                }

                return false;
            });
        }

        wf::get_core().connect(&grid_handle_move_signal_cb);
    }

    wf::signal::connection_t<wf::grid::grid_handle_move_signal> grid_handle_move_signal_cb =
        [=] (wf::grid::grid_handle_move_signal *ev)
    {
        ev->carried_out = true;
        wf::grid::slot_t new_slot_id = ev->operation == wf::grid::MOVE_OP_CLEAR_PREVIEW ?
            wf::grid::slot_t::SLOT_NONE :
            _calc_slot(ev->output, ev->input, snap_threshold, quarter_snap_threshold);

        if ((ev->operation == wf::grid::MOVE_OP_DROP) && new_slot_id)
        {
            ev->view->toplevel()->pending().tiled_edges = wf::grid::get_tiled_edges_for_slot(new_slot_id);
            auto input = ev->input;
            wf::geometry_t g = { input.x, input.y, 1, 1 };
            auto desired_size = _get_slot_dimensions(ev->view->get_output(), g, new_slot_id);
            ev->view->get_data_safe<wf_grid_slot_data>()->slot = new_slot_id;
            ensure_grid_view(ev->view)->adjust_target_geometry(
                adjust_for_workspace(ev->view->get_wset(), desired_size,
                    ev->view->get_output()->wset()->get_current_workspace()),
                ev->view->toplevel()->pending().tiled_edges);
            new_slot_id = wf::grid::slot_t::SLOT_NONE;
        }

        /* No changes in the slot, just return */
        if (slot.slot_id == new_slot_id)
        {
            return;
        }

        /* Destroy previous preview */
        if (slot.preview)
        {
            auto input = ev->input;
            slot.preview->set_target_geometry({input.x, input.y, 1, 1}, 0, true);
            slot.preview = nullptr;
        }

        slot.slot_id = new_slot_id;

        /* Show a preview overlay */
        if (new_slot_id)
        {
            auto input = ev->input;
            wf::geometry_t g = { input.x, input.y, 1, 1 };
            wf::geometry_t slot_geometry = _get_slot_dimensions(ev->output, g, new_slot_id);
            /* Unknown slot geometry, can't show a preview */
            if ((slot_geometry.width <= 0) || (slot_geometry.height <= 0))
            {
                return;
            }

            slot.preview = std::make_shared<wf::preview_indication_t>(
                wf::geometry_t{input.x, input.y, 1, 1}, ev->output, "move");
            slot.preview->set_target_geometry(slot_geometry, 1);
        }
    };

    void handle_new_output(wf::output_t *output) override
    {
        output->connect(&on_workarea_changed);
        output->connect(&on_maximize_signal);
        output->connect(&on_fullscreen_signal);
        output->connect(&on_tiled);
    }

    void handle_output_removed(wf::output_t *output) override
    {
        // no-op
    }

    void fini() override
    {
        fini_output_tracking();
    }

    bool can_adjust_view(wayfire_toplevel_view view)
    {
        const uint32_t req_actions = wf::VIEW_ALLOW_MOVE | wf::VIEW_ALLOW_RESIZE;
        const bool is_floating     = (view->get_allowed_actions() & req_actions) == req_actions;
        return is_floating && (view->get_output() != nullptr) && view->toplevel()->pending().mapped;
    }

    void handle_slot(wayfire_toplevel_view view, int slot, wf::point_t delta = {0, 0})
    {
        if (!can_adjust_view(view))
        {
            return;
        }

        view->get_data_safe<wf_grid_slot_data>()->slot = slot;
        auto slot_geometry = _get_slot_dimensions(view->get_output(), view->get_geometry(), slot) + delta;
        ensure_grid_view(view)->adjust_target_geometry(
            slot_geometry, wf::grid::get_tiled_edges_for_slot(slot));
    }

    wf::signal::connection_t<wf::workarea_changed_signal> on_workarea_changed =
        [=] (wf::workarea_changed_signal *ev)
    {
        for (auto& view : ev->output->wset()->get_views(wf::WSET_MAPPED_ONLY))
        {
            auto data = view->get_data_safe<wf_grid_slot_data>();

            /* Detect if the view was maximized outside of the grid plugin */
            auto wm = view->get_pending_geometry();
            if (view->pending_tiled_edges() && (wm.width == ev->old_workarea.width) &&
                (wm.height == ev->old_workarea.height))
            {
                data->slot = wf::grid::SLOT_CENTER;
            }

            if (!data->slot)
            {
                continue;
            }

            /* Workarea changed, and we have a view which is tiled into some slot.
             * We need to make sure it remains in its slot. So we calculate the
             * viewport of the view, and tile it there */
            auto output_geometry = ev->output->get_relative_geometry();

            int vx = std::floor(1.0 * wm.x / output_geometry.width);
            int vy = std::floor(1.0 * wm.y / output_geometry.height);

            handle_slot(view, data->slot, {vx *output_geometry.width, vy * output_geometry.height});
        }
    };

    wf::geometry_t adjust_for_workspace(std::shared_ptr<wf::workspace_set_t> wset,
        wf::geometry_t geometry, wf::point_t workspace)
    {
        auto delta_ws = workspace - wset->get_current_workspace();
        auto scr_size = wset->get_last_output_geometry().value();
        geometry.x += delta_ws.x * scr_size.width;
        geometry.y += delta_ws.y * scr_size.height;
        return geometry;
    }

    wf::signal::connection_t<wf::view_tile_request_signal> on_maximize_signal =
        [=] (wf::view_tile_request_signal *data)
    {
        if (data->carried_out || (data->desired_size.width <= 0) || !data->view->get_output() ||
            !data->view->get_wset() || !can_adjust_view(data->view))
        {
            return;
        }

        data->carried_out = true;
        uint32_t slot = wf::grid::get_slot_from_tiled_edges(data->edges);
        if (slot > 0)
        {
            data->desired_size = _get_slot_dimensions(data->view->get_output(), data->view->get_geometry(), slot);
        }

        data->view->get_data_safe<wf_grid_slot_data>()->slot = slot;
        ensure_grid_view(data->view)->adjust_target_geometry(
            adjust_for_workspace(data->view->get_wset(), data->desired_size, data->workspace),
            wf::grid::get_tiled_edges_for_slot(slot));
    };

    wf::signal::connection_t<wf::view_fullscreen_request_signal> on_fullscreen_signal =
        [=] (wf::view_fullscreen_request_signal *data)
    {
        static const std::string fs_data_name = "grid-saved-fs";
        if (data->carried_out || (data->desired_size.width <= 0) || !data->view->get_output() ||
            !data->view->get_wset() || !can_adjust_view(data->view))
        {
            return;
        }

        int32_t edges = -1;
        auto geom     = data->desired_size;

        if (!data->state && data->view->has_data<wf_grid_slot_data>())
        {
            uint32_t slot = data->view->get_data_safe<wf_grid_slot_data>()->slot;
            if (slot > 0)
            {
                geom  = wf::grid::get_slot_dimensions(data->view->get_output(), slot);
                edges = wf::grid::get_tiled_edges_for_slot(slot);
            }
        }

        data->carried_out = true;
        ensure_grid_view(data->view)->adjust_target_geometry(
            adjust_for_workspace(data->view->get_wset(), geom, data->workspace), edges);
    };

    wf::signal::connection_t<wf::view_tiled_signal> on_tiled = [=] (wf::view_tiled_signal *ev)
    {
        if (!ev->view->has_data<wf_grid_slot_data>())
        {
            return;
        }

        auto data = ev->view->get_data_safe<wf_grid_slot_data>();
        if (ev->new_edges != wf::grid::get_tiled_edges_for_slot(data->slot))
        {
            ev->view->erase_data<wf_grid_slot_data>();
        }
    };
};

DECLARE_WAYFIRE_PLUGIN(wayfire_grid_plus);
