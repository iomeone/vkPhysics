#include "server/nw_server_meta.hpp"
#include "srv_main.hpp"
#include "nw_server.hpp"
#include <common/net.hpp>
#include <common/game.hpp>
#include <common/event.hpp>
#include <common/string.hpp>
#include <common/meta_packet.hpp>
#include <common/game_packet.hpp>
#include <cstddef>

static flexible_stack_container_t<uint32_t> clients_to_send_chunks_to;

// Local server information
struct server_info_t {
    const char *server_name;
};

static server_info_t local_server_info;

static bool started_server = 0;

static void s_start_server(
    event_start_server_t *data) {
    clients_to_send_chunks_to.init(50);

    memset(g_net_data.dummy_voxels, CHUNK_SPECIAL_VALUE, sizeof(g_net_data.dummy_voxels));

    main_udp_socket_init(GAME_OUTPUT_PORT_SERVER);

    g_net_data.clients.init(NET_MAX_CLIENT_COUNT);

    started_server = 1;

    track_modification_history();

    g_net_data.acc_predicted_modifications.init();

    uint32_t sizeof_chunk_mod_pack = sizeof(chunk_modifications_t) * MAX_PREDICTED_CHUNK_MODIFICATIONS;

    g_net_data.chunk_modification_allocator.pool_init(
        sizeof_chunk_mod_pack,
        sizeof_chunk_mod_pack * NET_MAX_ACCUMULATED_PREDICTED_CHUNK_MODIFICATIONS_PER_PACK + 4);
}

// PT_CONNECTION_HANDSHAKE
static bool s_send_packet_connection_handshake(
    uint16_t client_id,
    event_new_player_t *player_info,
    uint32_t loaded_chunk_count) {
    packet_connection_handshake_t connection_handshake = {};
    connection_handshake.loaded_chunk_count = loaded_chunk_count;
    connection_handshake.player_infos = LN_MALLOC(full_player_info_t, g_net_data.clients.data_count);

    for (uint32_t i = 0; i < g_net_data.clients.data_count; ++i) {
        client_t *client = g_net_data.clients.get(i);
        if (client->initialised) {
            full_player_info_t *info = &connection_handshake.player_infos[connection_handshake.player_count];

            if (i == client_id) {
                info->name = client->name;
                info->client_id = client->client_id;
                info->ws_position = player_info->info.ws_position;
                info->ws_view_direction = player_info->info.ws_view_direction;
                info->ws_up_vector = player_info->info.ws_up_vector;
                info->ws_next_random_position = player_info->info.next_random_spawn_position;
                info->default_speed = player_info->info.default_speed;

                player_flags_t flags = {};
                flags.u32 = player_info->info.flags;
                flags.is_local = 1;

                info->flags = flags;
            }
            else {
                int32_t local_id = translate_client_to_local_id(i);
                player_t *p = get_player(local_id);
                info->name = client->name;
                info->client_id = client->client_id;
                info->ws_position = p->ws_position;
                info->ws_view_direction = p->ws_view_direction;
                info->ws_up_vector = p->ws_up_vector;
                info->ws_next_random_position = player_info->info.next_random_spawn_position;
                info->default_speed = p->default_speed;

                player_flags_t flags = {};
                flags.u32 = player_info->info.flags;
                flags.is_local = 0;
                
                info->flags = flags;
            }
            
            ++connection_handshake.player_count;
        }
    }

    packet_header_t header = {};
    header.current_tick = get_current_tick();
    header.flags.total_packet_size = packed_packet_header_size() + packed_connection_handshake_size(&connection_handshake);
    header.flags.packet_type = PT_CONNECTION_HANDSHAKE;

    serialiser_t serialiser = {};
    serialiser.init(header.flags.total_packet_size);

    serialise_packet_header(&header, &serialiser);
    serialise_connection_handshake(&connection_handshake, &serialiser);

    client_t *c = g_net_data.clients.get(client_id);

    if (send_to_client(&serialiser, c->address)) {
        LOG_INFOV("Sent handshake to client: %s\n", c->name);
        return 1;
    }
    else {
        LOG_INFOV("Failed to send handshake to client: %s\n", c->name);
        return 0;
    }
}

static constexpr uint32_t maximum_chunks_per_packet() {
    return ((65507 - sizeof(uint32_t)) / (sizeof(int16_t) * 3 + CHUNK_EDGE_LENGTH * CHUNK_EDGE_LENGTH * CHUNK_EDGE_LENGTH));
}

static void s_serialise_chunk(
    serialiser_t *serialiser,
    uint32_t *chunks_in_packet,
    voxel_chunk_values_t *values,
    uint32_t i) {
    voxel_chunk_values_t *current_values = &values[i];

    serialiser->serialise_int16(current_values->x);
    serialiser->serialise_int16(current_values->y);
    serialiser->serialise_int16(current_values->z);

    for (uint32_t v_index = 0; v_index < CHUNK_EDGE_LENGTH * CHUNK_EDGE_LENGTH * CHUNK_EDGE_LENGTH; ++v_index) {
        uint8_t current_voxel = current_values->voxel_values[v_index];
        if (current_voxel == 0) {
            uint32_t before_head = serialiser->data_buffer_head;

            uint32_t zero_count = 0;
            for (; current_values->voxel_values[v_index] == 0 && zero_count < 5; ++v_index, ++zero_count) {
                serialiser->serialise_uint8(0);
            }
            

            if (zero_count == 5) {
                for (; current_values->voxel_values[v_index] == 0; ++v_index, ++zero_count) {}

                serialiser->data_buffer_head = before_head;
                serialiser->serialise_uint8(CHUNK_SPECIAL_VALUE);
                serialiser->serialise_uint32(zero_count);
            }

            v_index -= 1;
        }
        else {
            serialiser->serialise_uint8(current_voxel);
        }
    }

    *chunks_in_packet = *chunks_in_packet + 1;
}

// PT_CHUNK_VOXELS
static void s_send_packet_chunk_voxels(
    client_t *client,
    voxel_chunk_values_t *values,
    uint32_t count) {
    packet_header_t header = {};
    header.flags.packet_type = PT_CHUNK_VOXELS;
    header.flags.total_packet_size = 15 * (3 * sizeof(int16_t) + CHUNK_EDGE_LENGTH * CHUNK_EDGE_LENGTH * CHUNK_EDGE_LENGTH);
    header.current_tick = get_current_tick();
    header.current_packet_count = g_net_data.current_packet;
    
    serialiser_t serialiser = {};
    serialiser.init(header.flags.total_packet_size);

    serialise_packet_header(&header, &serialiser);
    
    uint32_t chunks_in_packet = 0;

    uint8_t *chunk_count_byte = &serialiser.data_buffer[serialiser.data_buffer_head];

    // For now serialise 0
    serialiser.serialise_uint32(0);

    uint32_t chunk_values_start = serialiser.data_buffer_head;
    
    uint32_t index = clients_to_send_chunks_to.add();
    clients_to_send_chunks_to[index] = client->client_id;

    for (uint32_t i = 0; i < count; ++i) {
        s_serialise_chunk(&serialiser, &chunks_in_packet, values, i);

        if (serialiser.data_buffer_head + 3 * sizeof(int16_t) + CHUNK_EDGE_LENGTH * CHUNK_EDGE_LENGTH * CHUNK_EDGE_LENGTH > serialiser.data_buffer_size ||
            i + 1 == count) {
            // Need to send in new packet
            serialiser.serialise_uint32(chunks_in_packet, chunk_count_byte);
            client_chunk_packet_t *packet_to_save = &client->chunk_packets[client->chunk_packet_count++];
            packet_to_save->chunk_data = FL_MALLOC(uint8_t, serialiser.data_buffer_head);
            memcpy(packet_to_save->chunk_data, serialiser.data_buffer, serialiser.data_buffer_head);
            //packet_to_save->chunk_data = serialiser.data_buffer;
            packet_to_save->size = serialiser.data_buffer_head;

            serialiser.data_buffer_head = chunk_values_start;
            chunks_in_packet = 0;
        }
    }

    client->current_chunk_sending = 0;
}

// Sends handshake, and starts sending voxels
static void s_send_game_state_to_new_client(
    uint16_t client_id,
    event_new_player_t *player_info) {
    uint32_t loaded_chunk_count = 0;
    chunk_t **chunks = get_active_chunks(&loaded_chunk_count);

    if (s_send_packet_connection_handshake(
        client_id,
        player_info,
        loaded_chunk_count)) {
        client_t *client = g_net_data.clients.get(client_id);

        // Send chunk information
        uint32_t max_chunks_per_packet = maximum_chunks_per_packet();
        LOG_INFOV("Maximum chunks per packet: %i\n", max_chunks_per_packet);

        voxel_chunk_values_t *voxel_chunks = LN_MALLOC(voxel_chunk_values_t, loaded_chunk_count);

        uint32_t count = 0;
        for (uint32_t i = 0; i < loaded_chunk_count; ++i) {
            chunk_t *c = chunks[i];
            if (c) {
                voxel_chunks[count].x = c->chunk_coord.x;
                voxel_chunks[count].y = c->chunk_coord.y;
                voxel_chunks[count].z = c->chunk_coord.z;

                voxel_chunks[count].voxel_values = c->voxels;

                ++count;
            }
        }

        loaded_chunk_count = count;

        // Cannot send all of these at the same bloody time
        s_send_packet_chunk_voxels(client, voxel_chunks, loaded_chunk_count);
    }
}

// PT_PLAYER_JOINED
static void s_send_packet_player_joined(
    event_new_player_t *info) {
    packet_player_joined_t packet = {};
    packet.player_info.name = info->info.client_name;
    packet.player_info.client_id = info->info.client_id;
    packet.player_info.ws_position = info->info.ws_position;
    packet.player_info.ws_view_direction = info->info.ws_view_direction;
    packet.player_info.ws_up_vector = info->info.ws_up_vector;
    packet.player_info.default_speed = info->info.default_speed;
    packet.player_info.flags.is_local = 0;

    packet_header_t header = {};
    header.current_tick = get_current_tick();
    header.current_packet_count = g_net_data.current_packet;
    header.flags.total_packet_size = packed_packet_header_size() + packed_player_joined_size(&packet);
    header.flags.packet_type = PT_PLAYER_JOINED;
    
    serialiser_t serialiser = {};
    serialiser.init(header.flags.total_packet_size);
    
    serialise_packet_header(&header, &serialiser);
    serialise_player_joined(&packet, &serialiser);
    
    for (uint32_t i = 0; i < g_net_data.clients.data_count; ++i) {
        if (i != packet.player_info.client_id) {
            client_t *c = g_net_data.clients.get(i);
            if (c->initialised) {
                send_to_client(&serialiser, c->address);
            }
        }
    }
}

// PT_CONNECTION_REQUEST
static void s_receive_packet_connection_request(
    serialiser_t *serialiser,
    network_address_t address,
    event_submissions_t *events) {
    packet_connection_request_t request = {};
    deserialise_connection_request(&request, serialiser);

    uint32_t client_id = g_net_data.clients.add();

    LOG_INFOV("New client with ID %i\n", client_id);

    client_t *client = g_net_data.clients.get(client_id);
    
    client->initialised = 1;
    client->client_id = client_id;
    client->name = create_fl_string(request.name);
    client->address = address;
    client->received_first_commands_packet = 0;
    client->predicted_chunk_mod_count = 0;
    client->predicted_modifications = (chunk_modifications_t *)g_net_data.chunk_modification_allocator.allocate_arena();
    memset(client->predicted_modifications, 0, sizeof(chunk_modifications_t) * MAX_PREDICTED_CHUNK_MODIFICATIONS);
    
    event_new_player_t *event_data = FL_MALLOC(event_new_player_t, 1);
    event_data->info.client_name = client->name;
    event_data->info.client_id = client->client_id;
    // Need to calculate a random position
    // TODO: In future, make it so that there is like some sort of startup screen when joining a server (like choose team, etc..)
    event_data->info.ws_position = vector3_t(0.0f);
    event_data->info.ws_view_direction = vector3_t(1.0f, 0.0f, 0.0f);
    event_data->info.ws_up_vector = vector3_t(0.0f, 1.0f, 0.0f);
    event_data->info.default_speed = PLAYER_WALKING_SPEED;

    event_data->info.flags = 0;
    // Player starts of as dead
    // There are different ways of spawning the player: meteorite (basically uncontrollably flying into the world)
    // event_data->info.alive_state = 0;

    // Generate new position
    float x_rand = (float)(rand() % 100 + 100) * (rand() % 2 == 0 ? -1 : 1);
    float y_rand = (float)(rand() % 100 + 100) * (rand() % 2 == 0 ? -1 : 1);
    float z_rand = (float)(rand() % 100 + 100) * (rand() % 2 == 0 ? -1 : 1);
    event_data->info.next_random_spawn_position = vector3_t(x_rand, y_rand, z_rand);
    
    submit_event(ET_NEW_PLAYER, event_data, events);

    // Send game state to new player
    s_send_game_state_to_new_client(client_id, event_data);
    // Dispatch to all players newly joined player information
    s_send_packet_player_joined(event_data);
}

// PT_CLIENT_DISCONNECT
static void s_receive_packet_client_disconnect(
    serialiser_t *serialiser,
    uint16_t client_id,
    event_submissions_t *events) {
    (void)serialiser;
    LOG_INFO("Client disconnected\n");

    g_net_data.clients[client_id].initialised = 0;
    g_net_data.clients.remove(client_id);

    event_player_disconnected_t *data = FL_MALLOC(event_player_disconnected_t, 1);
    data->client_id = client_id;
    submit_event(ET_PLAYER_DISCONNECTED, data, events);

    serialiser_t out_serialiser = {};
    out_serialiser.init(100);
    packet_header_t header = {};
    header.current_tick = get_current_tick();
    header.current_packet_count = g_net_data.current_packet;
    header.flags.packet_type = PT_PLAYER_LEFT;
    header.flags.total_packet_size = packed_packet_header_size() + sizeof(uint16_t);

    serialise_packet_header(&header, &out_serialiser);
    out_serialiser.serialise_uint16(client_id);
    
    for (uint32_t i = 0; i < g_net_data.clients.data_count; ++i) {
        client_t *c = &g_net_data.clients[i];
        if (c->initialised) {
            send_to_client(&out_serialiser, c->address);
        }
    }
}

static void s_handle_chunk_modifications(
    packet_client_commands_t *commands,
    client_t *client) {
    merge_chunk_modifications(
        client->predicted_modifications,
        &client->predicted_chunk_mod_count,
        commands->chunk_modifications,
        commands->modified_chunk_count);
}

// PT_CLIENT_COMMANDS
static void s_receive_packet_client_commands(
    serialiser_t *serialiser,
    uint16_t client_id,
    uint64_t tick,
    event_submissions_t *events) {
    int32_t local_id = translate_client_to_local_id(client_id);
    player_t *p = get_player(local_id);

    if (p) {
        client_t *c = &g_net_data.clients[p->client_id];

        c->received_first_commands_packet = 1;

        packet_client_commands_t commands = {};
        deserialise_player_commands(&commands, serialiser);

        if (commands.requested_spawn) {
            event_spawn_t *spawn = FL_MALLOC(event_spawn_t, 1);
            spawn->client_id = client_id;
            //LOG_INFOV("Client %i spawned\n", client_id);
            submit_event(ET_SPAWN, spawn, events);

            // Generate a new random position next time player needs to spawn
            float x_rand = (float)(rand() % 100 + 100) * (rand() % 2 == 0 ? -1 : 1);
            float y_rand = (float)(rand() % 100 + 100) * (rand() % 2 == 0 ? -1 : 1);
            float z_rand = (float)(rand() % 100 + 100) * (rand() % 2 == 0 ? -1 : 1);
            p->next_random_spawn_position = vector3_t(x_rand, y_rand, z_rand);
        }
        
        if (commands.did_correction) {
            LOG_INFOV("Did correction: %s\n", glm::to_string(p->ws_position).c_str());
            c->waiting_on_correction = 0;
        }

        // Only process client commands if we are not waiting on a correction
        if (!c->waiting_on_correction) {
            // Tick at which client sent packet (since last game state snapshot dispatch)
            if (c->should_set_tick) {
                c->tick = tick;
                c->should_set_tick = 0;

                //LOG_INFOV("Received Tick %llu\n", (unsigned long long)c->tick);
            }

            for (uint32_t i = 0; i < commands.command_count; ++i) {
                //LOG_INFOV("Accumulated dt: %f\n", commands.actions[i].accumulated_dt);
                push_player_actions(p, &commands.actions[i], 1);
            }

            c->ws_predicted_position = commands.ws_final_position;
            c->ws_predicted_view_direction = commands.ws_final_view_direction;
            c->ws_predicted_up_vector = commands.ws_final_up_vector;
            c->ws_predicted_velocity = commands.ws_final_velocity;
            c->predicted_player_flags.u32 = commands.player_flags;

            if (c->predicted_player_flags.alive_state == PAS_DEAD) {
                // LOG_INFO("Player is now dead!\n");
            }
            
            // Process terraforming stuff
            if (commands.modified_chunk_count) {
                //LOG_INFOV("(Tick %llu) Received %i chunk modifications\n", (unsigned long long)tick, commands.modified_chunk_count);
                c->did_terrain_mod_previous_tick = 1;
                c->tick_at_which_client_terraformed = tick;

                if (commands.modified_chunk_count) {
#if 1
                    //printf("\n");
                    //LOG_INFOV("Predicted %i chunk modifications at tick %llu\n", commands.modified_chunk_count, (unsigned long long)tick);
                    for (uint32_t i = 0; i < commands.modified_chunk_count; ++i) {
                        //LOG_INFOV("In chunk (%i %i %i): \n", commands.chunk_modifications[i].x, commands.chunk_modifications[i].y, commands.chunk_modifications[i].z);
                        chunk_t *c_ptr = get_chunk(ivector3_t(commands.chunk_modifications[i].x, commands.chunk_modifications[i].y, commands.chunk_modifications[i].z));
                        for (uint32_t v = 0; v < commands.chunk_modifications[i].modified_voxels_count; ++v) {
                            uint8_t initial_value = c_ptr->voxels[commands.chunk_modifications[i].modifications[v].index];
                            if (c_ptr->history.modification_pool[commands.chunk_modifications[i].modifications[v].index] != CHUNK_SPECIAL_VALUE) {
                                //initial_value = c_ptr->history.modification_pool[commands.chunk_modifications[i].modifications[v].index];
                            }
                            if (initial_value != commands.chunk_modifications[i].modifications[v].initial_value) {
                                LOG_INFOV("(Voxel %i) INITIAL VALUES ARE NOT THE SAME: %i != %i\n", commands.chunk_modifications[i].modifications[v].index, (int32_t)initial_value, (int32_t)commands.chunk_modifications[i].modifications[v].initial_value);
                            }
                            //LOG_INFOV("- index %i | initial value %i | final value %i\n", (int32_t)commands.chunk_modifications[i].modifications[v].index, initial_value, (int32_t)commands.chunk_modifications[i].modifications[v].final_value);
                        }
                    }
#endif
                }
                
                s_handle_chunk_modifications(&commands, c);
            }
        }
    }
    else {
        // There is a problem
        LOG_ERROR("Player was not initialised yet, cannot process client commands!\n");
    }
}

static bool s_check_if_client_has_to_correct_state(
    player_t *p,
    client_t *c) {
    vector3_t dposition = glm::abs(p->ws_position - c->ws_predicted_position);
    vector3_t ddirection = glm::abs(p->ws_view_direction - c->ws_predicted_view_direction);
    vector3_t dup = glm::abs(p->ws_up_vector - c->ws_predicted_up_vector);
    vector3_t dvelocity = glm::abs(p->ws_velocity - c->ws_predicted_velocity);

    float precision = 0.000001f;
    bool incorrect_position = 0;
    if (dposition.x >= precision || dposition.y >= precision || dposition.z >= precision) {
        incorrect_position = 1;
    }

    bool incorrect_direction = 0;
    if (ddirection.x >= precision || ddirection.y >= precision || ddirection.z >= precision) {
        incorrect_direction = 1;
    }

    bool incorrect_up = 0;
    if (dup.x >= precision || dup.y >= precision || dup.z >= precision) {
        incorrect_up = 1;
    }

    bool incorrect_velocity = 0;
    if (dvelocity.x >= precision || dvelocity.y >= precision || dvelocity.z >= precision) {
        incorrect_velocity = 1;
        LOG_INFOV(
            "Need to correct velocity: %f %f %f <- %f %f %f\n",
            p->ws_velocity.x,
            p->ws_velocity.y,
            p->ws_velocity.z,
            c->ws_predicted_velocity.x,
            c->ws_predicted_velocity.y,
            c->ws_predicted_velocity.z);
    }

    bool incorrect_interaction_mode = 0;
    if (p->flags.interaction_mode != c->predicted_player_flags.interaction_mode) {
        LOG_INFO("Need to correct interaction mode\n");
        incorrect_interaction_mode = 1;
    }

    bool incorrect_alive_state = 0;
    if (p->flags.alive_state != c->predicted_player_flags.alive_state) {
        LOG_INFO("Need to correct alive state\n");
        incorrect_alive_state = 1;
    }

    return incorrect_position ||
        incorrect_direction ||
        incorrect_up ||
        incorrect_velocity ||
        incorrect_interaction_mode ||
        incorrect_alive_state;
}

static bool s_check_if_client_has_to_correct_terrain(
    client_t *c) {
    bool needs_to_correct = 0;

    for (uint32_t cm_index = 0; cm_index < c->predicted_chunk_mod_count; ++cm_index) {
        chunk_modifications_t *cm_ptr = &c->predicted_modifications[cm_index];
        cm_ptr->needs_to_correct = 0;

        chunk_t *c_ptr = get_chunk(ivector3_t(cm_ptr->x, cm_ptr->y, cm_ptr->z));

        bool chunk_has_mistake = 0;
        
        for (uint32_t vm_index = 0; vm_index < cm_ptr->modified_voxels_count; ++vm_index) {
            voxel_modification_t *vm_ptr = &cm_ptr->modifications[vm_index];

            uint8_t actual_value = c_ptr->voxels[vm_ptr->index];
            uint8_t predicted_value = vm_ptr->final_value;

            // Just one mistake can completely mess stuff up between the client and server
            if (actual_value != predicted_value) {
#if 0
                printf("(%i %i %i) Need to set (%i) %i -> %i\n", c_ptr->chunk_coord.x, c_ptr->chunk_coord.y, c_ptr->chunk_coord.z, vm_ptr->index, (int32_t)predicted_value, (int32_t)actual_value);
#endif

                // Change the predicted value and send this back to the client, and send this back to the client to correct
                vm_ptr->final_value = actual_value;

                chunk_has_mistake = 1;
            }
        }

        if (chunk_has_mistake) {
            LOG_INFOV("(Tick %llu)Above mistakes were in chunk (%i %i %i)\n", (unsigned long long)c->tick, c_ptr->chunk_coord.x, c_ptr->chunk_coord.y, c_ptr->chunk_coord.z);

            needs_to_correct = 1;
            cm_ptr->needs_to_correct = 1;
        }
    }

    return needs_to_correct;
}

// Send the voxel modifications
// It will the client's job to decide which voxels to interpolate between based on the
// fact that it knows which voxels it modified - it will not interpolate between voxels it knows to have modified
// in the time frame that concerns this state dispatch
static void s_add_chunk_modifications_to_game_state_snapshot(
    packet_game_state_snapshot_t *snapshot) {
    // Up to 300 chunks can be modified between game dispatches
    chunk_modifications_t *modifications = LN_MALLOC(chunk_modifications_t, NET_MAX_ACCUMULATED_PREDICTED_CHUNK_MODIFICATIONS_PER_PACK);

    uint32_t modification_count = fill_chunk_modification_array(modifications);

    snapshot->modified_chunk_count = modification_count;
    snapshot->chunk_modifications = modifications;
}

// PT_GAME_STATE_SNAPSHOT
static void s_send_packet_game_state_snapshot() {
#if NET_DEBUG || NET_DEBUG_VOXEL_INTERPOLATION
    printf("\n\n GAME STATE DISPATCH\n");
#endif
    
    packet_game_state_snapshot_t packet = {};
    packet.player_data_count = 0;
    packet.player_snapshots = LN_MALLOC(player_snapshot_t, g_net_data.clients.data_count);

    s_add_chunk_modifications_to_game_state_snapshot(&packet);
    
    for (uint32_t i = 0; i < g_net_data.clients.data_count; ++i) {
        client_t *c = &g_net_data.clients[i];

        if (c->initialised && c->received_first_commands_packet) {
            c->should_set_tick = 1;

            // Check if the data that the client predicted was correct, if not, force client to correct position
            // Until server is sure that the client has done a correction, server will not process this client's commands
            player_snapshot_t *snapshot = &packet.player_snapshots[packet.player_data_count];
            snapshot->flags = 0;

            int32_t local_id = translate_client_to_local_id(c->client_id);
            player_t *p = get_player(local_id);

            // Check if 
            bool has_to_correct_state = s_check_if_client_has_to_correct_state(p, c);
            // Check if client has to correct voxel modifications
            bool has_to_correct_terrain = s_check_if_client_has_to_correct_terrain(c);

            if (has_to_correct_state || has_to_correct_terrain) {
                if (c->waiting_on_correction) {
                    // TODO: Make sure to relook at this, so that in case of packet loss, the server doesn't just stall at this forever
                    LOG_INFO("Client needs to do correction, but did not receive correction acknowledgement, not sending correction\n");
                    snapshot->client_needs_to_correct_state = 0;
                    snapshot->server_waiting_for_correction = 1;
                }
                else {
                    // If there is a correction of any kind to do, force client to correct everything
                    c->waiting_on_correction = 1;

                    if (has_to_correct_terrain) {
                        snapshot->packet_contains_terrain_correction = 1;
                        c->send_corrected_predicted_voxels = 1;
                    }

                    LOG_INFOV("Client needs to revert to tick %llu\n\n", (unsigned long long)c->tick);
                    snapshot->client_needs_to_correct_state = has_to_correct_state || has_to_correct_terrain;
                    snapshot->server_waiting_for_correction = 0;
                    p->cached_player_action_count = 0;
                    p->player_action_count = 0;
                }
            }

            snapshot->client_id = c->client_id;
            snapshot->ws_position = p->ws_position;
            snapshot->ws_view_direction = p->ws_view_direction;
            snapshot->ws_next_random_spawn = p->next_random_spawn_position;
            snapshot->ws_velocity = p->ws_velocity;
            snapshot->ws_up_vector = p->ws_up_vector;
            snapshot->tick = c->tick;
            snapshot->terraformed = c->did_terrain_mod_previous_tick;
            snapshot->interaction_mode = p->flags.interaction_mode;
            snapshot->alive_state = p->flags.alive_state;
            
            if (snapshot->terraformed) {
                snapshot->terraform_tick = c->tick_at_which_client_terraformed;
            }

            // Reset
            c->did_terrain_mod_previous_tick = 0;
            c->tick_at_which_client_terraformed = 0;

            ++packet.player_data_count;
        }
    }

#if 0
    LOG_INFOV("Modified %i chunks\n", packet.modified_chunk_count);
    for (uint32_t i = 0; i < packet.modified_chunk_count; ++i) {
        LOG_INFOV("In chunk (%i %i %i): \n", packet.chunk_modifications[i].x, packet.chunk_modifications[i].y, packet.chunk_modifications[i].z);
        for (uint32_t v = 0; v < packet.chunk_modifications[i].modified_voxels_count; ++v) {
            LOG_INFOV("- index %i | initial value %i | final value %i\n", (int32_t)packet.chunk_modifications[i].modifications[v].index, (int32_t)packet.chunk_modifications[i].modifications[v].initial_value, (int32_t)packet.chunk_modifications[i].modifications[v].final_value);
        }
    }
#endif

    packet_header_t header = {};
    header.current_tick = get_current_tick();
    header.current_packet_count = g_net_data.current_packet;
    // Don't need to fill this
    header.client_id = 0;
    header.flags.packet_type = PT_GAME_STATE_SNAPSHOT;
    header.flags.total_packet_size = packed_packet_header_size() + packed_game_state_snapshot_size(&packet);

    serialiser_t serialiser = {};
    serialiser.init(header.flags.total_packet_size);

    serialise_packet_header(&header, &serialiser);

    // This is the packet for players that need correction
    serialise_game_state_snapshot(&packet, &serialiser);
    serialise_chunk_modifications(packet.chunk_modifications, packet.modified_chunk_count, &serialiser);
    
    uint32_t data_head_before = serialiser.data_buffer_head;
    
    for (uint32_t i = 0; i < g_net_data.clients.data_count; ++i) {
        client_t *c = &g_net_data.clients[i];

        if (c->send_corrected_predicted_voxels) {
            // Serialise
            LOG_INFOV("Need to correct %i chunks\n", c->predicted_chunk_mod_count);
            serialise_chunk_modifications(c->predicted_modifications, c->predicted_chunk_mod_count, &serialiser);
        }
        
        if (c->initialised && c->received_first_commands_packet) {
            send_to_client(&serialiser, c->address);
        }

        serialiser.data_buffer_head = data_head_before;
        
        // Clear client's predicted modification array
        c->predicted_chunk_mod_count = 0;
        c->send_corrected_predicted_voxels = 0;
    }

    reset_modification_tracker();

    //putchar('\n');
}

// PT_CHUNK_VOXELS
static void s_send_pending_chunks() {
    uint32_t to_remove_count = 0;
    uint32_t *to_remove = LN_MALLOC(uint32_t, clients_to_send_chunks_to.data_count);
    for (uint32_t i = 0; i < clients_to_send_chunks_to.data_count; ++i) {
        uint32_t client_id = clients_to_send_chunks_to[i];
        client_t *c_ptr = &g_net_data.clients[client_id];

        LOG_INFOV("Need to send %d chunks\n", c_ptr->chunk_packet_count);
        if (c_ptr->current_chunk_sending < c_ptr->chunk_packet_count) {
            client_chunk_packet_t *packet = &c_ptr->chunk_packets[c_ptr->current_chunk_sending];
            serialiser_t serialiser = {};
            serialiser.data_buffer_head = packet->size;
            serialiser.data_buffer = packet->chunk_data;
            serialiser.data_buffer_size = packet->size;
            if (send_to_client(&serialiser, c_ptr->address)) {
                LOG_INFO("Sent chunk packet to client\n");
            }

            // Free
            FL_FREE(packet->chunk_data);

            c_ptr->current_chunk_sending++;
        }
        else {
            to_remove[to_remove_count++] = i;
        }
    }

    for (uint32_t i = 0; i < to_remove_count; ++i) {
        clients_to_send_chunks_to.remove(to_remove[i]);
    }
}

static void s_tick_server(
    event_submissions_t *events) {
    static float snapshot_elapsed = 0.0f;
    snapshot_elapsed += srv_delta_time();
    
    if (snapshot_elapsed >= NET_SERVER_SNAPSHOT_OUTPUT_INTERVAL) {
        // Send commands to the server
        s_send_packet_game_state_snapshot();

        snapshot_elapsed = 0.0f;
    }

    // For sending chunks to new players
    static float world_elapsed = 0.0f;
    world_elapsed += srv_delta_time();

    if (world_elapsed >= NET_SERVER_CHUNK_WORLD_OUTPUT_INTERVAL) {
        s_send_pending_chunks();

        world_elapsed = 0.0f;
    }

    for (uint32_t i = 0; i < g_net_data.clients.data_count + 1; ++i) {
        network_address_t received_address = {};
        int32_t received = receive_from_client(
            g_net_data.message_buffer,
            sizeof(char) * NET_MAX_MESSAGE_SIZE,
            &received_address);

        if (received > 0) {
            serialiser_t in_serialiser = {};
            in_serialiser.data_buffer = (uint8_t *)g_net_data.message_buffer;
            in_serialiser.data_buffer_size = received;

            packet_header_t header = {};
            deserialise_packet_header(&header, &in_serialiser);

            switch(header.flags.packet_type) {

            case PT_CONNECTION_REQUEST: {
                s_receive_packet_connection_request(
                    &in_serialiser,
                    received_address,
                    events);
            } break;

            case PT_CLIENT_DISCONNECT: {
                s_receive_packet_client_disconnect(
                    &in_serialiser,
                    header.client_id,
                    events);
            } break;

            case PT_CLIENT_COMMANDS: {
                s_receive_packet_client_commands(
                    &in_serialiser,
                    header.client_id,
                    header.current_tick,
                    events);
            } break;

            }
        }
    }
}

static listener_t net_listener_id;

static void s_net_event_listener(
    void *object,
    event_t *event,
    event_submissions_t *events) {
    switch (event->type) {

    case ET_START_SERVER: {
        event_start_server_t *data = (event_start_server_t *)event->data;
        s_start_server(data);

        FL_FREE(data);
    } break;

    default: {
    } break;

    }
}

void nw_init(event_submissions_t *events) {
    net_listener_id = set_listener_callback(&s_net_event_listener, NULL, events);

    subscribe_to_event(ET_START_SERVER, net_listener_id, events);

    socket_api_init();

    g_net_data.message_buffer = FL_MALLOC(char, NET_MAX_MESSAGE_SIZE);

    // meta_socket_init();
    nw_init_meta_connection();
    nw_check_registration(events);
}

void nw_tick(event_submissions_t *events) {
    s_tick_server(events);
}
