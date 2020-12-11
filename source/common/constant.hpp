#pragma once

#define CHUNK_EDGE_LENGTH (16)
#define CHUNK_VOXEL_COUNT (CHUNK_EDGE_LENGTH * CHUNK_EDGE_LENGTH * CHUNK_EDGE_LENGTH)
#define CHUNK_MAX_LOADED_COUNT 2000
#define CHUNK_MAX_VOXEL_VALUE_F 254.0f
#define CHUNK_MAX_VERTICES_PER_CHUNK (5 * (CHUNK_EDGE_LENGTH - 1) * (CHUNK_EDGE_LENGTH - 1) * (CHUNK_EDGE_LENGTH - 1))
#define CHUNK_MAX_VOXEL_VALUE_I 254
#define CHUNK_SPECIAL_VALUE 255
#define CHUNK_SURFACE_LEVEL 70
#define CHUNK_BYTE_SIZE (CHUNK_VOXEL_COUNT * sizeof(voxel_t))

#define PLAYER_MAX_COUNT 50
#define PLAYER_SHAPE_SWITCH_DURATION 0.3f
#define PLAYER_MAX_ACTIONS_COUNT 100
#define PLAYER_SCALE 0.5f
#define PLAYER_TERRAFORMING_SPEED 200.0f
#define PLAYER_TERRAFORMING_RADIUS 3.0f
#define PLAYER_WALKING_SPEED 25.0f

#define PROJECTILE_MAX_ROCK_COUNT 1000
#define PROJECTILE_ROCK_SPEED 35.0f

#define GRAVITY_ACCELERATION 10.0f

#define NET_MAX_CLIENT_COUNT 50
#define NET_MAX_ACCUMULATED_PREDICTED_CHUNK_MODIFICATIONS_PACK_COUNT 60
#define NET_MAX_ACCUMULATED_PREDICTED_CHUNK_MODIFICATIONS_PER_PACK MAX_PREDICTED_CHUNK_MODIFICATIONS * 5
#define NET_MAX_MESSAGE_SIZE 65507
#define NET_MAX_AVAILABLE_SERVER_COUNT 1000
#define NET_CLIENT_COMMAND_OUTPUT_INTERVAL (1.0f / 25.0f)
#define NET_SERVER_SNAPSHOT_OUTPUT_INTERVAL (1.0f / 20.0f)
#define NET_SERVER_CHUNK_WORLD_OUTPUT_INTERVAL (1.0f / 40.0f)
#define NET_PING_INTERVAL 2.0f
#define NET_CLIENT_TIMEOUT 5.0f
