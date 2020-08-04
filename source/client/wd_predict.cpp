#include <common/game.hpp>
#include "net.hpp"
#include "wd_predict.hpp"
#include "wd_spectate.hpp"
#include <common/event.hpp>
#include <common/player.hpp>
#include <renderer/input.hpp>

static int32_t local_player;

static terraform_package_t local_current_terraform_package;

void wd_set_local_player(int32_t id) {
    local_player = id;
}

int32_t wd_get_local_player() {
    return local_player;
}

void wd_handle_local_player_input(float dt) {
    game_input_t *game_input = get_game_input();

    player_action_t actions;

    actions.bytes = 0;

    actions.accumulated_dt = 0.0f;

    actions.dt = dt;
    actions.dmouse_x = game_input->mouse_x - game_input->previous_mouse_x;
    actions.dmouse_y = game_input->mouse_y - game_input->previous_mouse_y;

    if (game_input->actions[GIAT_MOVE_FORWARD].state == BS_DOWN)
        actions.move_forward = 1;
    if (game_input->actions[GIAT_MOVE_LEFT].state == BS_DOWN)
        actions.move_left = 1;
    if (game_input->actions[GIAT_MOVE_BACK].state == BS_DOWN)
        actions.move_back = 1;
    if (game_input->actions[GIAT_MOVE_RIGHT].state == BS_DOWN)
        actions.move_right = 1;
    if (game_input->actions[GIAT_TRIGGER4].state == BS_DOWN)
        actions.jump = 1;
    if (game_input->actions[GIAT_TRIGGER6].state == BS_DOWN)
        actions.crouch = 1;
    if (game_input->actions[GIAT_TRIGGER1].state == BS_DOWN)
        actions.trigger_left = 1;
    if (game_input->actions[GIAT_TRIGGER2].state == BS_DOWN)
        actions.trigger_right = 1;
    if (game_input->actions[GIAT_TRIGGER5].instant == BS_DOWN)
        actions.switch_shapes = 1;
    if (game_input->actions[GIAT_TRIGGER7].instant == BS_DOWN)
        actions.flashlight = 1;

    actions.tick = get_current_tick();
    
    player_t *local_player_ptr = get_player(local_player);

    if (local_player) {
        if (local_player_ptr->flags.alive_state == PAS_ALIVE) {
            push_player_actions(local_player_ptr, &actions, 0);
        }
        else {
            push_player_actions(wd_get_spectator(), &actions, 0);
        }
    }
    else {
        push_player_actions(wd_get_spectator(), &actions, 0);
    }
}

void wd_execute_player_actions(player_t *player, event_submissions_t *events) {
    for (uint32_t i = 0; i < player->player_action_count; ++i) {
        player_action_t *action = &player->player_actions[i];

        execute_action(player, action);

        if (connected_to_server()) {
            // If this is local player, need to cache these commands to later send to server
            player->camera_distance.animate(action->dt);
            player->camera_fov.animate(action->dt);

            local_current_terraform_package = player->terraform_package;
            if (player->flags.interaction_mode != PIM_STANDING) {
                local_current_terraform_package.ray_hit_terrain = 0;
            }

            vector3_t up_diff = player->next_camera_up - player->current_camera_up;
            if (glm::dot(up_diff, up_diff) > 0.00001f) {
                player->current_camera_up = glm::normalize(player->current_camera_up + up_diff * action->dt * 1.5f);
            }

            if (connected_to_server()) {
                if (player->cached_player_action_count < PLAYER_MAX_ACTIONS_COUNT * 2) {
                    player->cached_player_actions[player->cached_player_action_count++] = *action;
                }
                else {
                    LOG_WARNING("Too many cached player actions\n");
                }
            }

            if (player->flags.alive_state == PAS_DEAD) {
                submit_event(ET_LAUNCH_GAME_MENU_SCREEN, NULL, events);
                submit_event(ET_ENTER_SERVER_META_MENU, NULL, events);
            }
        }

        if (player->flags.alive_state == PAS_DEAD)
            break;
    }
}

void wd_predict_state(event_submissions_t *events) {
    player_t *player = get_player(local_player);
    
    wd_execute_player_actions(player, events);
}

void wd_kill_local_player(struct event_submissions_t *events) {
    LOG_INFO("Local player just died\n");

    submit_event(ET_LAUNCH_GAME_MENU_SCREEN, NULL, events);
    submit_event(ET_ENTER_SERVER_META_MENU, NULL, events);

    player_t *local_player_ptr = get_player(local_player);
    local_player_ptr->flags.alive_state = PAS_DEAD;
}

struct terraform_package_t *wd_get_local_terraform_package() {
    return &local_current_terraform_package;
}
