#include "wd_core.hpp"
#include <common/player.hpp>

static player_t *spectator;

void wd_create_spectator() {
    spectator = FL_MALLOC(player_t, 1);
    memset(spectator, 0, sizeof(player_t));
    spectator->default_speed = 40.0f;
    spectator->ws_position = vector3_t(3.7f, -136.0f, -184.0f);
    spectator->ws_view_direction = vector3_t(0.063291, 0.438437, 0.896531);
    spectator->ws_up_vector = vector3_t(0.0f, 1.0f, 0.0f);
    spectator->flags.interaction_mode = PIM_FLOATING;
    spectator->camera_fov.current = 60.0f;
    spectator->current_camera_up = vector3_t(0.0f, 1.0f, 0.0f);
    spectator->idx_in_chunk_list = -1;
}

player_t *wd_get_spectator() {
    return spectator;
}
