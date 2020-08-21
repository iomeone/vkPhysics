#include "ui.hpp"
#include "dr_rsc.hpp"
#include "wd_core.hpp"
#include "fx_post.hpp"
#include "fx_fade.hpp"
#include "gm_mode.hpp"
#include "cl_event.hpp"
#include "nw_client.hpp"
#include "cl_render.hpp"
#include <common/game.hpp>
#include <common/files.hpp>
#include <common/event.hpp>
#include "nw_client_meta.hpp"
#include <renderer/input.hpp>
#include <common/allocators.hpp>

static bool running;

// Core listener of in-game events - main code in cl_event.cpp
static listener_t core_listener;

// All event submissions go here
static event_submissions_t events;

static float dt;

// Starts a fade in effect into the main menu screen
static void s_open() {
    // Launch fade effect immediately
    event_begin_fade_effect_t *fade_info = FL_MALLOC(event_begin_fade_effect_t, 1);
    fade_info->dest_value = 1.0f;
    fade_info->duration = 6.0f;
    fade_info->trigger_count = 0;
    submit_event(ET_BEGIN_FADE, fade_info, &events);
    
    // submit_event(ET_LAUNCH_MAIN_MENU_SCREEN, NULL, &events);
    submit_event(ET_ENTER_MAIN_MENU, NULL, &events);

    event_start_client_t *start_client_data = FL_MALLOC(event_start_client_t, 1);
    start_client_data->client_name = "Some shit";
    submit_event(ET_START_CLIENT, start_client_data, &events);
}

static void s_run() {
    while (running) {
        poll_input_events(&events);
        translate_raw_to_game_input();
        dispatch_events(&events);

        LN_CLEAR();

        frame_command_buffers_t frame = cl_prepare_frame();

        // // Tick whatever game mode was bound
        gm_tick(
            frame.render_command_buffer,
            frame.transfer_command_buffer,
            frame.ui_command_buffer,
            &events);

        // This needs to always happen - quite an important part of the loop
        // Handles transitions between different game modes
        fx_tick_fade_effect(&events);

        cl_finish_frame();
        dt = surface_delta_time();
    }
}

// Entry point for client program
int32_t main(
    int32_t argc,
    char *argv[]) {
    global_linear_allocator_init((uint32_t)megabytes(30));
    srand(time(NULL));
    core_listener = set_listener_callback(cl_game_event_listener, NULL, &events);
    running = 1;
    files_init();
    cl_subscribe_to_events(core_listener, &events);

    game_input_settings_init();
    renderer_init();
    fx_fader_init();
    ui_init(&events);
    nw_init(&events);

    s_open();

    cl_command_buffers_init();

    dr_resources_init();
    game_memory_init();
    wd_init(&events);

    // Initialise game modes
    gm_modes_init(&events);
    // Bind main menu
    gm_bind(GMT_MAIN_MENU);

    fx_get_frame_info()->debug_window = 1;

    // Check if the user has registered and can actually join servers
    nw_check_registration(&events);

    s_run();

    dispatch_events(&events);
    dispatch_events(&events);

    return 0;
}

void cl_terminate() {
    running = 0;
    submit_event(ET_LEAVE_SERVER, NULL, &events);
}

float cl_delta_time() {
    return dt;
}