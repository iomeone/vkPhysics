#pragma once

void ui_init(
    struct event_submissions_t *events);

void handle_ui_input(
    event_submissions_t *events);

void tick_ui(
    struct event_submissions_t *events);
