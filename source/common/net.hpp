#pragma once

#include "common/containers.hpp"
#include "common/weapon.hpp"
#include "tools.hpp"
#include "player.hpp"
#include "socket.hpp"
#include "constant.hpp"
#include "serialiser.hpp"
#include <bits/stdint-uintn.h>

#define MAX_PREDICTED_CHUNK_MODIFICATIONS 20
#define MAX_PREDICTED_VOXEL_MODIFICATIONS_PER_CHUNK 250

//#define NET_DEBUG_VOXEL_INTERPOLATION 1
//#define NET_DEBUG 1
//#define NET_DEBUG_TERRAFORMING 1

struct voxel_modification_t {
    uint16_t index;
    uint8_t final_value;

    // When the client sends this to the server, initial value will be stored
    // When the server sends this to the client, color value will be stored
    union {
        uint8_t initial_value;
        uint8_t color;
    };
};

struct chunk_modifications_t {
    int16_t x, y, z;
    uint32_t modified_voxels_count;
    // Due to alignment and padding issues, it's best to store the data like so
    voxel_modification_t modifications[MAX_PREDICTED_VOXEL_MODIFICATIONS_PER_CHUNK];
    voxel_color_t colors[MAX_PREDICTED_VOXEL_MODIFICATIONS_PER_CHUNK];

    union {
        uint8_t flags;
        struct {
            uint8_t needs_to_correct: 1;
        };
    };
};

struct client_chunk_packet_t {
    voxel_t *chunk_data;
    uint32_t size;
};


struct accumulated_predicted_modification_t {
    // Tick at which client sent these to the server
    uint64_t tick;
    // Accumulated predicted chunk modifications
    uint32_t acc_predicted_chunk_mod_count;
    // These will stay until server returns with game state dispatch confirming no errors have happened
    chunk_modifications_t *acc_predicted_modifications;
};

struct game_server_t {
    const char *server_name;
    uint32_t ipv4_address;
};

struct available_servers_t {
    uint32_t server_count;
    game_server_t *servers;

    hash_table_t<uint32_t, 50, 5, 5> name_to_server;
};

#define MAX_CLIENT_PACKET_STORAGE 5

struct packet_t {
    network_address_t address;
    uint32_t byte_size;
    void *data;
};

struct net_data_t {
    uint64_t current_packet;
    char *message_buffer;
    stack_container_t<client_t> clients;
    // This stores the index of the modification in the chunk_modifications_t struct's array
    uint8_t dummy_voxels[CHUNK_VOXEL_COUNT];
    arena_allocator_t chunk_modification_allocator;
    circular_buffer_array_t<
        accumulated_predicted_modification_t,
        NET_MAX_ACCUMULATED_PREDICTED_CHUNK_MODIFICATIONS_PACK_COUNT> acc_predicted_modifications;
    accumulated_predicted_modification_t merged_recent_modifications;
    available_servers_t available_servers;

    FILE *log_file;

    uint32_t current_packet_count;
    packet_t packets[MAX_CLIENT_PACKET_STORAGE];

    // List of "predicted" projectile hits from local client
};


extern net_data_t g_net_data;

void main_udp_socket_init(uint16_t output_port);
// When something has been received, make sure (that isn't ping),
// Make sure to dispatch it to the main thread when needed
void start_client_udp_thread();
void stop_client_udp_thread();

void meta_socket_init();
bool send_to_game_server(serialiser_t *serialiser, network_address_t address);
int32_t receive_from_game_server(char *message_buffer, uint32_t max_size, network_address_t *addr);
int32_t receive_from_client(char *message_buffer, uint32_t max_size, network_address_t *addr);
bool send_to_meta_server(serialiser_t *serialiser);
bool send_to_client(serialiser_t *serialiser, network_address_t address);
void acc_predicted_modification_init(accumulated_predicted_modification_t *apm_ptr, uint64_t tick);
accumulated_predicted_modification_t *add_acc_predicted_modification();
void check_incoming_meta_server_packets(event_submissions_t *events);
void flag_modified_chunks(chunk_modifications_t *modifications, uint32_t count);
void unflag_modified_chunks(chunk_modifications_t *modifications, uint32_t count);
void fill_dummy_voxels(chunk_modifications_t *modifications);
void unfill_dummy_voxels(chunk_modifications_t *modifications);
uint32_t fill_chunk_modification_array_with_initial_values(chunk_modifications_t *modifications);
uint32_t fill_chunk_modification_array_with_colors(chunk_modifications_t *modifications);
accumulated_predicted_modification_t *accumulate_history();
void merge_chunk_modifications(
    chunk_modifications_t *dst,
    uint32_t *dst_count,
    chunk_modifications_t *src,
    uint32_t src_count);

#if NET_DEBUG_TERRAFORMING
template <typename ...T> void debug_log(
    const char *format,
    bool print_to_console,
    T ...values) {
    fprintf(g_net_data.log_file, format, values...);

    if (print_to_console) {
        printf(format, values...);
    }
}
#else
template<typename ...T> void debug_log(
    const char *format,
    bool print_to_console,
    T ...values) {
    // Don't do anything
}
#endif
