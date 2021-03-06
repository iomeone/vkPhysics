#include "game.hpp"
#include "chunk.hpp"
#include "player.hpp"
#include "constant.hpp"
#include <glm/gtx/projection.hpp>
#include <glm/gtx/string_cast.hpp>

static void s_init_player_weapons(player_t *p) {
    p->weapon_count = 2;
    p->weapons[0].init(40, 40, firing_type_t::SEMI_AUTOMATIC, bullet_type_t::PROJECTILE, weapon_type_t::ROCKS, 5.0f);
    p->weapons[1].init(0, 0, firing_type_t::INVALID, bullet_type_t::INVALID, weapon_type_t::TERRAFORMER, 0);
}

static void s_set_default_values(
    player_init_info_t *init_info,
    player_t *player) {
    player->ws_position = init_info->ws_position;
    player->ws_view_direction = init_info->ws_view_direction;
    player->ws_up_vector = init_info->ws_up_vector;
    player->player_action_count = 0;
    player->default_speed = init_info->default_speed;
    player->next_random_spawn_position = init_info->next_random_spawn_position;
    player->ball_speed = 0.0f;
    player->ws_velocity = vector3_t(0.0f);
    player->idx_in_chunk_list = -1;
    player->health = 200;
    player->snapshot_before = player->snapshot_before = 0;

    // FOR NOW: just have the default weapon setup

    player->selected_weapon = 0;
    s_init_player_weapons(player);
}

void fill_player_info(player_t *player, player_init_info_t *init_info) {
    if (init_info->client_name) {
        player->name = init_info->client_name;
        player->client_id = init_info->client_id;
        // Now the network module can use w_get_player_from_client_id to get access to player directly
        g_game->client_to_local_id_map[init_info->client_id] = player->local_id;
    }

    s_set_default_values(init_info, player);

    memset(player->player_actions, 0, sizeof(player->player_actions));
    player->accumulated_dt = 0.0f;

    player->flags.u32 = init_info->flags;
}

void push_player_actions(player_t *player, player_action_t *action, bool override_adt) {
    if (player->player_action_count < PLAYER_MAX_ACTIONS_COUNT) {
        if (override_adt) {
            player->player_actions[player->player_action_count++] = *action;
        }
        else {
            player->accumulated_dt += action->dt;

            float max_change = 1.0f * player->accumulated_dt * PLAYER_TERRAFORMING_SPEED;
            int32_t max_change_i = (int32_t)max_change;

            if (max_change_i < 2) {
                action->accumulated_dt = 0.0f;
            }
            else {
                action->accumulated_dt = player->accumulated_dt;
                player->accumulated_dt = 0.0f;
            }

            player->player_actions[player->player_action_count++] = *action;
        }
    }
    else {
        // ...
        LOG_WARNING("Too many player actions\n");
    }
}

void handle_shape_switch(player_t *player, bool switch_shapes, float dt) {
    if (switch_shapes) {
        // If already switching shapes
        if (player->switching_shapes)
            player->shape_switching_time = SHAPE_SWITCH_ANIMATION_TIME - player->shape_switching_time;
        else
            player->shape_switching_time = 0.0f;

        if (player->flags.interaction_mode == PIM_STANDING) {
            player->flags.interaction_mode = PIM_BALL;
            player->switching_shapes = 1;
        }
        else if (player->flags.interaction_mode == PIM_BALL) {
            player->flags.interaction_mode = PIM_STANDING;
            player->switching_shapes = 1;

            // Need to set animation to the "STOP" animation if the velocity exceeds a certain value
            if (player->frame_displacement / dt > 4.0f)
                player->animated_state = PAS_STOP_FAST;
        }
    }

    if (player->switching_shapes)
        player->shape_switching_time += g_game->dt;

    if (player->shape_switching_time > PLAYER_SHAPE_SWITCH_DURATION) {
        player->shape_switching_time = 0.0f;
        player->switching_shapes = 0;
    }
} 

static void s_handle_weapon_switch(
    player_t *player,
    player_action_t *actions) {
    if (actions->switch_weapons) {
        if (actions->next_weapon == 0b111) {
            // Just cycle through to next weapon
            player->selected_weapon = (player->selected_weapon + 1) % player->weapon_count;
            LOG_INFOV("Player chose weapon: %d\n", player->selected_weapon);
        }
        else if (actions->next_weapon < player->weapon_count) {
            player->selected_weapon = actions->next_weapon;
            LOG_INFOV("Player chose weapon: %d\n", player->selected_weapon);
        }
    }
}

// Special abilities like 
static void s_execute_player_triggers(
    player_t *player,
    player_action_t *player_actions) {
    s_handle_weapon_switch(player, player_actions);

    weapon_t *weapon = &player->weapons[player->selected_weapon];

    switch (weapon->type) {
    case weapon_type_t::ROCKS: {
        // Spawn rock
        if (player_actions->trigger_left && weapon->elapsed > weapon->recoil_time) {
            weapon->elapsed = 0.0f;
            
            // TODO: Do check to see if the player can shoot...
            uint32_t ref_idx = weapon->active_projs.add();

            uint32_t rock_idx = g_game->rocks.spawn(
                compute_player_view_position(player),
                player->ws_view_direction * PROJECTILE_ROCK_SPEED,
                player->ws_up_vector,
                player->client_id,
                ref_idx,
                player->selected_weapon);

            weapon->active_projs[ref_idx].initialised = 1;
            weapon->active_projs[ref_idx].idx = rock_idx;
        }

        player->terraform_package.ray_hit_terrain = 0;
    } break;

        // Special case
    case weapon_type_t::TERRAFORMER: {
        player->terraform_package = cast_terrain_ray(
            compute_player_view_position(player),
            player->ws_view_direction,
            10.0f,
            player->terraform_package.color);

        if (player_actions->trigger_left)
            terraform(TT_DESTROY, player->terraform_package, PLAYER_TERRAFORMING_RADIUS, PLAYER_TERRAFORMING_SPEED, player_actions->accumulated_dt);
        if (player_actions->trigger_right)
            terraform(TT_BUILD, player->terraform_package, PLAYER_TERRAFORMING_RADIUS, PLAYER_TERRAFORMING_SPEED, player_actions->accumulated_dt);

        if (player_actions->flashlight) {
            player->flags.flashing_light ^= 1;
        }
    } break;
    }
}

static void s_execute_player_direction_change(
    player_t *player,
    player_action_t *player_actions) {
    vector2_t delta = vector2_t(player_actions->dmouse_x, player_actions->dmouse_y);

    static constexpr float SENSITIVITY = 15.0f;

    vector3_t res = player->ws_view_direction;

    float x_angle = glm::radians(-delta.x) * SENSITIVITY * player_actions->dt;
    float y_angle = glm::radians(-delta.y) * SENSITIVITY * player_actions->dt;
                
    res = matrix3_t(glm::rotate(x_angle, player->ws_up_vector)) * res;
    vector3_t rotate_y = glm::cross(res, player->ws_up_vector);
    res = matrix3_t(glm::rotate(y_angle, rotate_y)) * res;

    res = glm::normalize(res);
                
    player->ws_view_direction = res;
}

static void s_execute_player_floating_movement(
    player_t *player,
    player_action_t *actions) {
    vector3_t right = glm::normalize(glm::cross(player->ws_view_direction, player->ws_up_vector));
    vector3_t forward = glm::normalize(glm::cross(player->ws_up_vector, right));

    if (actions->move_forward)
        player->ws_position += forward * actions->dt * player->default_speed;
    if (actions->move_left)
        player->ws_position -= right * actions->dt * player->default_speed;
    if (actions->move_back)
        player->ws_position -= forward * actions->dt * player->default_speed;
    if (actions->move_right)
        player->ws_position += right * actions->dt * player->default_speed;
    if (actions->jump)
        player->ws_position += player->ws_up_vector * actions->dt * player->default_speed;
    if (actions->crouch)
        player->ws_position -= player->ws_up_vector * actions->dt * player->default_speed;
}

enum movement_resolution_flags_t {
    MRF_ADOPT_GRAVITY = 1 << 0,
    MRF_GRAVITY_CHECK_INCLINATION = 1 << 1,
    MRF_ABRUPT_STOP = 1 << 2,
    MRF_CAP_SPEED = 1 << 3,
};

struct force_values_t {
    // This will be very high for standing mode
    float friction;
    // This will be very high for standing mode too (maybe?)
    float movement_acceleration;
    float gravity;
    float maximum_walking_speed;
};

enum movement_change_type_t {
    MCT_STOP, MCT_TOO_FAST, MCT_MADE_MOVEMENT
};

static bool s_create_acceleration_vector(
    player_t *player,
    player_action_t *actions,
    movement_resolution_flags_t flags,
    force_values_t *force_values,
    vector3_t *acceleration) {
    movement_axes_t axes = compute_movement_axes(player->ws_view_direction, player->ws_up_vector);

    bool made_movement = 0;
    vector3_t final_acceleration = vector3_t(0.0f);
    
    if (actions->move_forward) {
        final_acceleration += axes.forward * actions->dt * player->default_speed;
        made_movement = 1;
    }
    if (actions->move_left) {
        final_acceleration -= axes.right * actions->dt * player->default_speed;
        made_movement = 1;
    }
    if (actions->move_back) {
        final_acceleration -= axes.forward * actions->dt * player->default_speed;
        made_movement = 1;
    }
    if (actions->move_right) {
        final_acceleration += axes.right * actions->dt * player->default_speed;
        made_movement = 1;
    }
    if (actions->jump) {
        final_acceleration += axes.up * actions->dt * player->default_speed;
        made_movement = 1;
    }
    if (actions->crouch) {
        final_acceleration -= axes.up * actions->dt * player->default_speed;
        made_movement = 1;
    }

    *acceleration = final_acceleration;

    return made_movement;
}

static void s_apply_forces(
    player_t *player,
    player_action_t *actions,
    force_values_t *force_values,
    movement_resolution_flags_t flags,
    const vector3_t &movement_acceleration) {
    player->ws_velocity += movement_acceleration * actions->dt * force_values->movement_acceleration;

    // Do some check between standing and ball mode
    if (flags & MRF_GRAVITY_CHECK_INCLINATION) {
        if (glm::dot(player->ws_up_vector, player->ws_surface_normal) < 0.7f || player->flags.contact == PCS_IN_AIR) {
            player->ws_velocity += -player->ws_up_vector * force_values->gravity * actions->dt;
        }
        else {
        }
    }
    else {
        player->ws_velocity += -player->ws_up_vector * force_values->gravity * actions->dt;
    }

    if (player->flags.contact == PCS_ON_GROUND) {
        player->ws_velocity += -player->ws_velocity * actions->dt * force_values->friction;
    }
    else {
    }
}

static void s_resolve_player_movement(
    player_t *player,
    player_action_t *actions,
    force_values_t *force_values,
    int32_t flags) {
    vector3_t acceleration = vector3_t(0.0f);
    bool made_movement = s_create_acceleration_vector(player, actions, (movement_resolution_flags_t)flags, force_values, &acceleration);
        
    if (made_movement && glm::dot(acceleration, acceleration) != 0.0f) {
        acceleration = glm::normalize(acceleration);
        player->flags.moving = 1;
    }
    else {
        player->flags.moving = 0;
    }

    if (!made_movement && player->flags.contact == PCS_ON_GROUND && flags & MRF_ABRUPT_STOP) {
        //player->ws_velocity = vector3_t(0.0f);
    }

    s_apply_forces(player, actions, force_values, (movement_resolution_flags_t)flags, acceleration);

    terrain_collision_t collision = {};
    collision.ws_size = vector3_t(PLAYER_SCALE);
    collision.ws_position = player->ws_position;
    collision.ws_velocity = player->ws_velocity * actions->dt;
    collision.es_position = collision.ws_position / collision.ws_size;
    collision.es_velocity = collision.ws_velocity / collision.ws_size;

    vector3_t ws_new_position = collide_and_slide(&collision) * PLAYER_SCALE;

    if (player->flags.contact != PCS_IN_AIR) {
        float len = glm::length(ws_new_position - player->ws_position);
        if (len < 0.00001f) {
            player->frame_displacement = 0.0f;
        }
        else {
            player->frame_displacement = len;
        }
    }

    player->ws_position = ws_new_position;

    if (collision.detected) {
        vector3_t normal = glm::normalize(collision.es_surface_normal * PLAYER_SCALE);
        player->ws_surface_normal = normal;

        if (player->flags.contact == PCS_IN_AIR) {
            if (glm::abs(glm::dot(player->ws_velocity, player->ws_velocity)) == 0.0f) {
                float previous_velocity_length = glm::length(player->ws_velocity);
                movement_axes_t new_axes = compute_movement_axes(player->ws_view_direction, player->ws_up_vector);
                player->ws_velocity = glm::normalize(glm::proj(player->ws_velocity, new_axes.forward)) * previous_velocity_length * 0.2f;
            }
        }

        if (flags & MRF_ADOPT_GRAVITY) {
            player->ws_up_vector = normal;
            player->next_camera_up = normal;
        }

        float vdotv = glm::dot(player->ws_velocity, player->ws_velocity);
        if (glm::abs(vdotv) == 0.0f) {
            player->ws_velocity = vector3_t(0.0f);
        }
        else {
            // Apply normal force
            player->ws_velocity += player->ws_up_vector * force_values->gravity * actions->dt;
        }
        player->flags.contact = PCS_ON_GROUND;
    }
    else {
        player->flags.contact = PCS_IN_AIR;
    }
}

static void s_execute_standing_player_movement(
    player_t *player,
    player_action_t *actions) {
    if (player->flags.contact == PCS_ON_GROUND) {
        if (player->animated_state == PAS_STOP_FAST && player->frame_displacement / actions->dt > 0.002f) {
            // Need to slow down
            actions->move_back = 0;
            actions->move_forward = 0;
            actions->move_left = 0;
            actions->move_right = 0;
            actions->jump = 0;
        }
        else if (actions->move_forward) {
            player->animated_state = PAS_RUNNING;
        }
        else if (actions->move_back) {
            player->animated_state = PAS_BACKWALKING;
        }
        else if (actions->move_left) {
            player->animated_state = PAS_LEFT_WALKING;
        }
        else if (actions->move_right) {
            player->animated_state = PAS_RIGHT_WALKING;
        }
        else {
            player->animated_state = PAS_IDLE;
        }

        if (actions->jump) {
            player->ws_velocity += player->ws_up_vector * 4.0f;

            player->animated_state = PAS_JUMPING_UP;

            player->flags.contact = PCS_IN_AIR;
        }
    }

    force_values_t forces = {};
    forces.friction = 5.0f;
    forces.movement_acceleration = 8.5f;
    forces.gravity = GRAVITY_ACCELERATION;
    forces.maximum_walking_speed = player->default_speed * 0.8;

    s_resolve_player_movement(
        player,
        actions,
        &forces,
        MRF_GRAVITY_CHECK_INCLINATION | MRF_ABRUPT_STOP | MRF_CAP_SPEED);
}

static void s_execute_ball_player_movement(
    player_t *player,
    player_action_t *actions) {
    force_values_t forces = {};
    forces.friction = 1.0f;
    forces.movement_acceleration = 10.0f;
    forces.gravity = 10.0f;
    forces.maximum_walking_speed = player->default_speed;
    s_resolve_player_movement(player, actions, &forces, MRF_ADOPT_GRAVITY | MRF_CAP_SPEED);
}

static void s_accelerate_meteorite_player(
    player_t *player,
    player_action_t *actions) {
    // Need to set player's up vector depending on direction it is flying
    vector3_t right = glm::normalize(glm::cross(player->ws_view_direction, player->ws_up_vector));
    player->ws_up_vector = glm::normalize(glm::cross(right, player->ws_view_direction));

    player->next_camera_up = player->ws_up_vector;

    static const float METEORITE_ACCELERATION = 25.0f;
    static const float MAX_METEORITE_SPEED = 25.0f;

    // Apply air resistance
    player->ws_velocity -= (player->ws_velocity * 0.1f) * actions->dt;

    vector3_t final_velocity = player->ws_velocity * actions->dt;

    vector3_t player_scale = vector3_t(PLAYER_SCALE);

    terrain_collision_t collision = {};
    collision.ws_size = player_scale;
    collision.ws_position = player->ws_position;
    collision.ws_velocity = final_velocity;
    collision.es_position = collision.ws_position / collision.ws_size;
    collision.es_velocity = collision.ws_velocity / collision.ws_size;

    player->ws_position = collide_and_slide(&collision) * player_scale;
    player->ws_velocity = (collision.es_velocity * player_scale) / actions->dt;

    if (collision.detected) {
        // Change interaction mode to ball
        player->flags.interaction_mode = PIM_BALL;
        player->camera_distance.set(1, 10.0f, player->camera_distance.current, 1.0f);
        player->camera_fov.set(1, 60.0f, player->camera_fov.current);

        vector3_t normal = glm::normalize(collision.es_surface_normal * player_scale);
        
        player->next_camera_up = normal;
        player->ws_up_vector = normal;

        player->flags.contact = PCS_ON_GROUND;
    }
    else {
        player->flags.contact = PCS_IN_AIR;

        if (glm::dot(player->ws_velocity, player->ws_velocity) < MAX_METEORITE_SPEED * MAX_METEORITE_SPEED) {
            //player->meteorite_speed += METEORITE_ACCELERATION * actions->dt;
            player->ws_velocity += player->ws_view_direction * METEORITE_ACCELERATION * actions->dt;
        }
        else {
            player->ws_velocity = glm::normalize(player->ws_velocity) * MAX_METEORITE_SPEED;
        }
    }
}

static void s_check_player_dead(
    player_action_t *actions,
    player_t *player) {
    // Start checking for death, if the velocity vector
    // is pretty aligned with the up vector (but down) of the player
    // because if it is, that means that the velocity vector won't be able to change too much.
    // (gravity has already pulled the player out of any sort of movement range)
    if (player->flags.contact == PCS_IN_AIR) {
        vector3_t normalized_velocity = glm::normalize(player->ws_velocity);

        float vdu = glm::dot(normalized_velocity, glm::normalize(-player->ws_up_vector));

        if (vdu > 0.9f) {
            // Do ray cast to check if there are chunks underneath the player
            uint32_t ray_step_count = 10;
            vector3_t current_ray_position = player->ws_position;
            current_ray_position += 16.0f * normalized_velocity;
            for (uint32_t i = 0; i < ray_step_count; ++i) {
                ivector3_t chunk_coord = space_voxel_to_chunk(space_world_to_voxel(current_ray_position));

                chunk_t *c = g_game->access_chunk(chunk_coord);
                if (c) {
                    // Might not die, reset timer
                    player->death_checker = 0.0f;

                    break;
                }

                current_ray_position += 16.0f * normalized_velocity;
            }

            player->death_checker += actions->dt;

            if (player->death_checker > 5.0f) {
                // The player is dead
                LOG_INFO("Player has just died\n");

                player->flags.alive_state = PAS_DEAD;
                player->frame_displacement = 0.0f;
            }
        }
    }
    else {
        player->death_checker = 0.0f;
    }
}

void execute_action(player_t *player, player_action_t *action) {
    // Shape switching can happen regardless of the interaction mode
    handle_shape_switch(player, action->switch_shapes, action->dt);

    switch (player->flags.interaction_mode) {
            
    case PIM_METEORITE: {
        s_execute_player_direction_change(player, action);
        s_accelerate_meteorite_player(player, action);
    } break;

        // FOR NOW STANDING AND BALL ARE EQUIVALENT ///////////////////////
    case PIM_STANDING: {
        s_execute_player_triggers(player, action);
        s_execute_player_direction_change(player, action);
        s_execute_standing_player_movement(player, action);

        s_check_player_dead(action, player);
    } break;

    case PIM_BALL: {
        s_execute_player_direction_change(player, action);
        s_execute_ball_player_movement(player, action);

        s_check_player_dead(action, player);
    } break;

    case PIM_FLOATING: {
        s_execute_player_triggers(player, action);
        s_execute_player_direction_change(player, action);
        s_execute_player_floating_movement(player, action);
    } break;

    default: {
    } break;

    }

    for (uint32_t i = 0; i < player->weapon_count; ++i) {
        player->weapons[i].elapsed += action->dt;
    }

    update_player_chunk_status(player);
}

void update_player_chunk_status(player_t *player) {
    // If the player has been added to a chunk that hadn't been created at the time
    if (player->idx_in_chunk_list == -1) {
        chunk_t *c = g_game->access_chunk(player->chunk_coord);

        if (c) {
            uint32_t idx = c->players_in_chunk.add();
            c->players_in_chunk[idx] = player->local_id;
            player->idx_in_chunk_list = idx;
        }
    }

    // Update the chunk in which the player is in
    ivector3_t new_chunk_coord = space_voxel_to_chunk(space_world_to_voxel(player->ws_position));

    if (
        new_chunk_coord.x != player->chunk_coord.x ||
        new_chunk_coord.y != player->chunk_coord.y ||
        new_chunk_coord.z != player->chunk_coord.z) {
        // Chunk is different

        // Remove player from previous chunk
        if (player->idx_in_chunk_list != -1) {
            chunk_t *c = g_game->access_chunk(player->chunk_coord);
            c->players_in_chunk.remove(player->idx_in_chunk_list);

            player->idx_in_chunk_list = -1;
        }

        player->chunk_coord = new_chunk_coord;

        chunk_t *c = g_game->access_chunk(new_chunk_coord);

        if (c) {
            uint32_t idx = c->players_in_chunk.add();
            c->players_in_chunk[idx] = player->local_id;
            player->idx_in_chunk_list = idx;
        }
    }
}

vector3_t compute_player_view_position(const player_t *p) {
    return p->ws_position + p->ws_up_vector * PLAYER_SCALE * 2.0f;
}

bool collide_sphere_with_standing_player(
    const vector3_t &target,
    const vector3_t &target_up,
    const vector3_t &center,
    float radius) {
    float player_height = PLAYER_SCALE * 2.0f;

    // Check collision with 2 spheres
    float sphere_scale = player_height * 0.5f;
    vector3_t body_low = target + (target_up * player_height * 0.22f);
    vector3_t body_high = target + (target_up * player_height * 0.75f);

    vector3_t body_low_diff = body_low - center;
    vector3_t body_high_diff = body_high - center;

    float dist2_low = glm::dot(body_low_diff, body_low_diff);
    float dist2_high = glm::dot(body_high_diff, body_high_diff);

    float dist_min = radius + sphere_scale;
    float dist_min2 = dist_min * dist_min;

    if (dist2_low < dist_min2 || dist2_high < dist_min2) {
        return true;
    }
    else {
        return false;
    }
}

bool collide_sphere_with_rolling_player(const vector3_t &target, const vector3_t &center, float radius) {
    float dist_min = radius + PLAYER_SCALE;
    float dist_min2 = dist_min * dist_min;

    vector3_t diff = target - center;
    float dist_to_player2 = glm::dot(diff, diff);

    if (dist_to_player2 < dist_min2) {
        return true;
    }
    else {
        return false;
    }
}

bool collide_sphere_with_player(
    const player_t *p,
    const vector3_t &center,
    float radius) {
    if (p->flags.interaction_mode == PIM_STANDING ||
        p->flags.interaction_mode == PIM_FLOATING) {
        return collide_sphere_with_standing_player(
            p->ws_position,
            p->ws_up_vector,
            center,
            radius);
    }
    else {
        return collide_sphere_with_rolling_player(
            p->ws_position,
            center,
            radius);
    }

    return false;
}
