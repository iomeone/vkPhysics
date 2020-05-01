#include "net.hpp"
#include "world.hpp"
#include "engine.hpp"
#include "w_internal.hpp"
#include <common/log.hpp>
#include <renderer/input.hpp>
#include <renderer/renderer.hpp>

static world_t world;

static void s_add_player_from_info(
    player_init_info_t *init_info) {
    player_t *p = w_add_player(&world);
        
    if (init_info->client_data) {
        p->name = init_info->client_data->name;
        p->client_id = init_info->client_data->client_id;
        // Now the network module can use w_get_player_from_client_id to get access to player directly
        w_link_client_id_to_local_id(p->client_id, p->local_id, &world);
    }

    p->ws_position = init_info->ws_position;
    p->ws_view_direction = init_info->ws_view_direction;
    p->ws_up_vector = init_info->ws_up_vector;
    p->player_action_count = 0;
    p->default_speed = init_info->default_speed;
    p->next_random_spawn_position = init_info->next_random_spawn_position;
    memset(p->player_actions, 0, sizeof(p->player_actions));

    p->accumulated_dt = 0.0f;

    p->flags = init_info->flags;
    
    if (init_info->is_local) {
        // If this is the local player (controlled by mouse and keyboard, need to cache all player actions to send to the server)
        // Only bind camera if player is alive
        if (init_info->alive_state == PAS_ALIVE) {
            // This binds the camera
            w_set_local_player(p->local_id, &world);
        }
        else {
            w_set_local_player(-1, &world);
        }
        
        p->cached_player_action_count = 0;
        p->cached_player_actions = FL_MALLOC(player_actions_t, MAX_PLAYER_ACTIONS * 2);

        p->is_remote = 0;
        p->is_local = 1;
    }
    else {
        p->is_remote = 1;
        p->is_local = 0;

        // Initialise remote snapshots
        p->remote_snapshots.init();
        p->elapsed = 0.0f;
    }

    LOG_INFOV("Added player %i: %s\n", p->local_id, p->name);
}

static void s_world_event_listener(
    void *,
    event_t *event) {
    switch(event->type) {

    case ET_ENTER_SERVER: {
        LOG_INFO("Entering server world\n");

        world.in_server = 1;

        // Reinitialise chunks / players
        w_clear_players(&world);
        w_clear_chunk_world(&world);

        event_enter_server_t *data = (event_enter_server_t *)event->data;

        for (uint32_t i = 0; i < data->info_count; ++i) {
            s_add_player_from_info(&data->infos[i]);
        }

        FL_FREE(data->infos);
        FL_FREE(event->data);
    } break;

    case ET_LEAVE_SERVER: {
        world.in_server = 0;
        w_clear_players(&world);
        w_clear_chunk_world(&world);
    } break;

    case ET_SPAWN: {
        event_spawn_t *data = (event_spawn_t *)event->data;
        uint32_t id = data->client_id;
        player_t *p = get_player(id);
        p->ws_position = p->next_random_spawn_position;
        p->ws_view_direction = glm::normalize(-p->ws_position);
        // Calculate up vector
        vector3_t right = glm::cross(p->ws_view_direction, vector3_t(0.0f, 1.0f, 0.0f));
        p->ws_up_vector = glm::cross(right, p->ws_view_direction);
        p->alive_state = PAS_ALIVE;

        if (p->is_local) {
            w_set_local_player(p->local_id, &world);
        }
    } break;
        
    case ET_NEW_PLAYER: {
        event_new_player_t *data = (event_new_player_t *)event->data;

        s_add_player_from_info(&data->info);

        FL_FREE(event->data);
    } break;

#if 0
    case ET_CHUNK_VOXEL_PACKET: {
        event_chunk_voxel_packet_t *data = (event_chunk_voxel_packet_t *)event->data;
        packet_chunk_voxels_t *packet = data->packet;

        for (uint32_t i = 0; i < packet->chunk_in_packet_count; ++i) {
            voxel_chunk_values_t *c_values = &packet->values[i];
            ivector3_t coord = ivector3_t(c_values->x, c_values->y, c_values->z);
            chunk_t *c = w_get_chunk(coord, &world);

            memcpy(c->voxels, c_values->voxel_values, CHUNK_EDGE_LENGTH * CHUNK_EDGE_LENGTH * CHUNK_EDGE_LENGTH);
            c->flags.has_to_update_vertices = 1;
        }

        LOG_INFOV("Loaded %i chunks\n", packet->chunk_in_packet_count);

        FL_FREE(data);
    } break;
#endif

    case ET_PLAYER_DISCONNECTED: {
        if (world.in_server) {
            event_player_disconnected_t *data = (event_player_disconnected_t *)event->data;

            player_t *p = w_get_player_from_client_id(data->client_id, &world);
            
            if (p) {
                w_destroy_player(p->local_id, &world);
            }

            FL_FREE(event->data);
        }
    } break;

    case ET_STARTED_RECEIVING_INITIAL_CHUNK_DATA: {
        w_toggle_mesh_update_wait(1, &world);
    } break;

    case ET_FINISHED_RECEIVING_INITIAL_CHUNK_DATA: {
        w_toggle_mesh_update_wait(0, &world);
    } break;

    case ET_SET_CHUNK_HISTORY_TRACKER: {
        event_set_chunk_history_tracker_t *data = (event_set_chunk_history_tracker_t *)event->data;
        world.track_history = data->value;

        FL_FREE(data);
    } break;

    default: {
    } break;

    }
}

static listener_t world_listener;

void world_init(
    event_submissions_t *events) {
    // terrain_collision_t collision_test = {};
    // collision_test.ws_size = vector3_t(1.0f);
    // collision_test.ws_position = vector3_t(0.0f, 1.0f, 0.0f);
    // collision_test.ws_velocity = vector3_t(1.0f, -1.0f, 1.0f) * 2.0f;
    // collision_test.es_position = collision_test.ws_position / collision_test.ws_size;
    // collision_test.es_velocity = collision_test.ws_velocity / collision_test.ws_size;

    // collision_triangle_t triangle = {};
    // triangle.vertices[0] = vector3_t(12.0f, 0.0f, 10.0f);
    // triangle.vertices[2] = vector3_t(1.0f, 0.0f, 10.0f);
    // triangle.vertices[1] = vector3_t(1.0f, 0.0f, -10.0f);
    // test_collision(&collision_test, &triangle);

    world_listener = set_listener_callback(s_world_event_listener, NULL, events);
    subscribe_to_event(ET_ENTER_SERVER, world_listener, events);
    subscribe_to_event(ET_NEW_PLAYER, world_listener, events);
    subscribe_to_event(ET_LEAVE_SERVER, world_listener, events);
    subscribe_to_event(ET_PLAYER_DISCONNECTED, world_listener, events);
#if 0
    subscribe_to_event(ET_CHUNK_VOXEL_PACKET, world_listener, events);
#endif
    subscribe_to_event(ET_STARTED_RECEIVING_INITIAL_CHUNK_DATA, world_listener, events);
    subscribe_to_event(ET_FINISHED_RECEIVING_INITIAL_CHUNK_DATA, world_listener, events);
    subscribe_to_event(ET_SET_CHUNK_HISTORY_TRACKER, world_listener, events);
    subscribe_to_event(ET_SPAWN, world_listener, events);

    memset(&world, 0, sizeof(world_t));

    w_players_data_init();
    w_player_world_init(&world);

    w_chunks_data_init();
    w_chunk_world_init(&world, 4);
}

void destroy_world() {
    w_clear_chunk_world(&world);
    w_destroy_chunk_data();
}

void handle_world_input() {
    game_input_t *game_input = get_game_input();
    
    w_handle_input(game_input, surface_delta_time(), &world);
}

void tick_world(
    event_submissions_t *events) {
    (void)events;
    w_tick_chunks(logic_delta_time());
    w_tick_players(&world);
}

void gpu_sync_world(
    VkCommandBuffer render_command_buffer,
    VkCommandBuffer transfer_command_buffer) {
    w_players_gpu_sync_and_render(
        render_command_buffer,
        transfer_command_buffer,
        &world);

    w_chunk_gpu_sync_and_render(
        render_command_buffer,
        transfer_command_buffer,
        &world);
    
    if (render_command_buffer != VK_NULL_HANDLE) {
        render_environment(render_command_buffer);
    }
}

eye_3d_info_t create_eye_info() {
    eye_3d_info_t info = {};

    player_t *player = w_get_local_player(&world);

    if (!player) {
        player = w_get_spectator(&world);
    }
    
    info.position = player->ws_position;
    info.direction = player->ws_view_direction;
    info.up = player->ws_up_vector;

    info.fov = 60.0f;
    info.near = 0.01f;
    info.far = 10000.0f;
    info.dt = surface_delta_time();

    return info;
}

lighting_info_t create_lighting_info() {
    lighting_info_t info = {};
    
    info.ws_directional_light = vector4_t(0.1f, 0.422f, 0.714f, 0.0f);
    /*info.ws_light_positions[0] = vector4_t(player.position, 1.0f);
    info.ws_light_directions[0] = vector4_t(player.direction, 0.0f);
    info.light_colors[0] = vector4_t(100.0f);*/
    info.lights_count = 0;

    return info;
}

player_t *get_player(
    uint16_t client_id) {
    return w_get_player_from_client_id(client_id, &world);
}

chunk_t *get_chunk(
    ivector3_t coord) {
    return w_get_chunk(coord, &world);
}

chunk_t *access_chunk(
    ivector3_t coord) {
    return w_access_chunk(coord, &world);
}

chunk_t **get_active_chunks(
    uint32_t *count) {
    *count = world.chunks.data_count;
    return world.chunks.data;
}

stack_container_t<player_t *> &DEBUG_get_players() {
    return world.players;
}

chunk_t **get_modified_chunks(
    uint32_t *count) {
    *count = world.modified_chunk_count;
    return world.modified_chunks;
}

void reset_modification_tracker() {
    for (uint32_t i = 0; i < world.modified_chunk_count; ++i) {
        chunk_t *c = world.modified_chunks[i];

        c->flags.made_modification = 0;

        for (int32_t v = 0; v < c->history.modification_count; ++v) {
            c->history.modification_pool[c->history.modification_stack[v]] = SPECIAL_VALUE;
        }

        c->history.modification_count = 0;
    }

    world.modified_chunk_count = 0;
}

void set_chunk_history_tracker_value(
    bool value) {
    world.track_history = value;
}
