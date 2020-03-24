#pragma once

#include <common/tools.hpp>
#include <common/containers.hpp>
#include <renderer/renderer.hpp>

#define CHUNK_EDGE_LENGTH 16
#define MAX_VOXEL_VALUE_F 254.0f
#define MAX_VOXEL_VALUE_I 254
#define MAX_VERTICES_PER_CHUNK 5 * (CHUNK_EDGE_LENGTH - 1) * (CHUNK_EDGE_LENGTH - 1) * (CHUNK_EDGE_LENGTH - 1)

// Push constant
struct chunk_render_data_t {
    matrix4_t model_matrix;
};

struct chunk_render_t {
    mesh_t mesh;
    chunk_render_data_t render_data;
    gpu_buffer_t chunk_vertices_gpu_buffer;
    bool should_do_gpu_sync = 0;
};

struct chunk_t {
    struct flags_t {
        uint32_t made_modification: 1;
    } flags;
    
    uint32_t chunk_stack_index;
    ivector3_t xs_bottom_corner;
    ivector3_t chunk_coord;

    uint8_t voxels[CHUNK_EDGE_LENGTH * CHUNK_EDGE_LENGTH * CHUNK_EDGE_LENGTH];

    chunk_render_t *render;
};

uint32_t get_voxel(
    uint32_t x,
    uint32_t y,
    uint32_t z);

void w_chunk_init(
    chunk_t *chunk,
    uint32_t chunk_stack_index,
    const ivector3_t &chunk_coord);

void w_chunk_render_init(
    chunk_t *chunk,
    const vector3_t &ws_position,
    const vector3_t &ws_size);

// Would call this at end of game or when chunk is out of chunk load radius.
void w_destroy_chunk_render(
    chunk_t *chunk);

// Returns NULL
// Usage: chunk = w_destroy_chunk(chun);
chunk_t *w_destroy_chunk(
    chunk_t *chunk);

struct chunk_pointer_t {
    // Index into the loaded_chunk_list
    uint32_t chunk_index;
};

// Max loaded chunks for now (loaded chunk = chunk with active voxels)
#define MAX_LOADED_CHUNKS 1000

struct chunk_world_t {
    uint32_t loaded_radius;

    // List of chunks
    // Works like a stack
    stack_container_t<chunk_t *> chunks;

    hash_table_t<uint32_t, 40, 5, 5> chunk_indices;
};

uint32_t w_hash_chunk_coord(
    const ivector3_t &coord);

void w_chunk_data_init();

void w_chunk_world_init(
    chunk_world_t *world,
    uint32_t loaded_radius);

void w_chunk_gpu_sync(
    chunk_world_t *world);

// Any function suffixed with _m means that the function will cause chunks to be added to a list needing gpusync
void w_add_sphere_m(
    const vector3_t &ws_center,
    float ws_radius);

ivector3_t w_convert_world_to_voxel(
    const vector3_t &ws_position);

ivector3_t w_convert_voxel_to_chunk(
    const ivector3_t &vs_position);

vector3_t w_convert_chunk_to_world(
    const ivector3_t &chunk_coord);

ivector3_t w_convert_voxel_to_local_chunk(
    const ivector3_t &vs_position);

chunk_t *w_get_chunk(
    const ivector3_t &coord,
    chunk_world_t *world);
