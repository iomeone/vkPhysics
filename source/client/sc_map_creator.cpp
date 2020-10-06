#include "client/cl_view.hpp"
#include "client/u_popup.hpp"
#include "common/event.hpp"
#include "common/map.hpp"
#include "ui.hpp"
#include "cl_main.hpp"
#include "fx_post.hpp"
#include "wd_core.hpp"
#include "sc_scene.hpp"
#include "wd_predict.hpp"
#include "wd_spectate.hpp"
#include "dr_draw_scene.hpp"
#include "sc_map_creator.hpp"

#include <common/player.hpp>
#include <cstddef>

enum submode_t {
    S_IN_GAME,
    S_PAUSE,
    S_INVALID
};

static submode_t submode;

// 
static map_t *map;

void sc_map_creator_init(listener_t listener, event_submissions_t *events) {
    subscribe_to_event(ET_PRESSED_ESCAPE, listener, events);
    subscribe_to_event(ET_BEGIN_MAP_EDITING, listener, events);
    subscribe_to_event(ET_CREATE_NEW_MAP, listener, events);
    subscribe_to_event(ET_DONT_CREATE_NEW_MAP, listener, events);
}

void sc_bind_map_creator() {
    fx_disable_blur();
    fx_enable_ssao();

    // Set local player to the spectator
    wd_set_local_player(-1);
}

static void s_handle_input(event_submissions_t *events) {
    switch (submode) {

    case S_IN_GAME: {
        wd_input(cl_delta_time());
    } break;

    case S_PAUSE: {
        handle_ui_input(events);
    } break;

    default: {
    } break;

    }
}

void sc_map_creator_tick(VkCommandBuffer render, VkCommandBuffer transfer, VkCommandBuffer ui, event_submissions_t *events) {
    s_handle_input(events);

    // The world always gets ticked - when menus get displayed, the world has to keep being simulated
    wd_execute_player_actions(wd_get_spectator(), events);
    wd_tick(events);

    dr_draw_game(render, transfer);

    tick_ui(events);
    render_submitted_ui(transfer, ui);

    eye_3d_info_t *eye_info = sc_get_eye_info();
    player_t *player = NULL;

    player = wd_get_spectator();

    eye_info->up = player->current_camera_up;

    eye_info->fov = player->camera_fov.current;
    eye_info->near = 0.01f;
    eye_info->far = 10000.0f;
    eye_info->dt = cl_delta_time();

    lighting_info_t *light_info = sc_get_lighting_info();
    light_info->ws_directional_light = vector4_t(0.1f, 0.422f, 0.714f, 0.0f);
    light_info->lights_count = 0;
}

void sc_handle_map_creator_event(void *object, event_t *event, event_submissions_t *events) {
    switch (event->type) {
    case ET_BEGIN_MAP_EDITING: {
        clear_ui_panels();

        submode = S_IN_GAME;
        cl_change_view_type(GVT_IN_GAME);

        event_enter_map_creator_t *event_data = (event_enter_map_creator_t *)event->data;

        map_t *map = load_map(event_data->map_path);
        if (map->is_new) {
            // Create popup
            ui_popup_t *popup = u_add_popup(2);
            u_push_popup_section_text(popup, "Create new?");

            const char *texts[] = { "Yes", "No" };
            void (* procs[2])(ui_popup_t *, event_submissions_t *) = {
                [] (ui_popup_t *, event_submissions_t *events) { submit_event(ET_CREATE_NEW_MAP, NULL, events); },
                [] (ui_popup_t *, event_submissions_t *events) { submit_event(ET_DONT_CREATE_NEW_MAP, NULL, events); }
            };

            u_push_popup_section_button_double(popup, texts, procs);

            u_prepare_popup_for_render(popup);

            cl_change_view_type(GVT_MENU);
            submode = S_PAUSE;
        }
        else {
            // TODO: Load map contents from file
        }

        FL_FREE(event->data);
    } break;

    case ET_CREATE_NEW_MAP: {
        // Add map to map names
    } break;

    case ET_DONT_CREATE_NEW_MAP: {
        // Exit map creator
    } break;

    case ET_PRESSED_ESCAPE: {
        if (submode == S_IN_GAME) {
            push_ui_panel(USI_GAME_MENU);
            cl_change_view_type(GVT_MENU);
            submode = S_PAUSE;
        }
        else {
            pop_ui_panel();
            cl_change_view_type(GVT_IN_GAME);
            submode = S_IN_GAME;
        }
    } break;

    default: {
    } break;
    }
}
