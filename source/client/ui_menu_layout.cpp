#include "ui_core.hpp"
#include "ui_menu_layout.hpp"
#include <renderer/input.hpp>
#include <renderer/renderer.hpp>

void menu_layout_t::init(
    menu_click_handler_t *handlers,
    const char **widget_image_paths,
    VkDescriptorSet *descriptor_sets,
    uint32_t iwidget_count) {
    if (iwidget_count > MAX_MENU_WIDGETS) {
        assert(0);
    }

    main_box.init(
        RT_CENTER,
        2.0f,
        ui_vector2_t(0.0f, 0.0f),
        ui_vector2_t(0.8f, 0.8f),
        NULL,
        0x46464646);

    widget_count = iwidget_count;
    current_button = widget_count;
    current_open_menu = widget_count;

    float current_button_y = 0.0f;
    float button_size = 0.25f;

    for (uint32_t i = 0; i < widget_count; ++i) {
        procs[i] = handlers[i];

        menu_widget_t *widget = &widgets[i];

        widget->box.init(
            RT_RIGHT_UP,
            1.0f,
            ui_vector2_t(0.0f, current_button_y),
            ui_vector2_t(button_size, button_size),
            &main_box,
            MENU_WIDGET_NOT_HOVERED_OVER_BACKGROUND_COLOR);

        widget->image_box.init(
            RT_RELATIVE_CENTER,
            1.0f,
            ui_vector2_t(0.0f, 0.0f),
            ui_vector2_t(0.8f, 0.8f),
            &widget->box,
            MENU_WIDGET_NOT_HOVERED_OVER_ICON_COLOR);
    
        if (widget_image_paths[i]) {
            widget->texture = create_texture(
                widget_image_paths[i],
                VK_FORMAT_R8G8B8A8_UNORM,
                NULL,
                0,
                0,
                VK_FILTER_LINEAR);
        }
        else {
            if (descriptor_sets) {
                widget->texture.descriptor = descriptor_sets[i];
            }
            else {
                assert(0);
            }
        }

        widget->color.init(
            MENU_WIDGET_NOT_HOVERED_OVER_BACKGROUND_COLOR,
            MENU_WIDGET_HOVERED_OVER_BACKGROUND_COLOR,
            MENU_WIDGET_NOT_HOVERED_OVER_ICON_COLOR,
            MENU_WIDGET_HOVERED_OVER_ICON_COLOR);

        current_button_y -= button_size;
    }

    current_menu.init(
        RT_RIGHT_UP,
        1.75f,
        ui_vector2_t(-0.125f, 0.0f),
        ui_vector2_t(1.0f, 1.0f),
        &main_box,
        MENU_WIDGET_CURRENT_MENU_BACKGROUND);

    menu_slider_x_max_size = current_menu.gls_current_size.to_fvec2().x;
    menu_slider_y_max_size = current_menu.gls_current_size.to_fvec2().y;

    current_menu.gls_current_size.fx = 0.0f;

    menu_in_out_transition = 0;
}

void menu_layout_t::submit() {
    for (uint32_t i = 0; i < widget_count; ++i) {
        mark_ui_textured_section(widgets[i].texture.descriptor);
        push_textured_ui_box(&widgets[i].image_box);

        push_colored_ui_box(&widgets[i].box);
    }

    push_reversed_colored_ui_box(
        &current_menu,
        vector2_t(menu_slider_x_max_size, menu_slider_y_max_size) * 2.0f);
}

#define MENU_SLIDER_SPEED 0.3f

bool menu_layout_t::input(
    event_submissions_t *events,
    raw_input_t *input) {
    menu_slider.animate(surface_delta_time());
    current_menu.gls_current_size.fx = menu_slider.current;

    float cursor_x = input->cursor_pos_x, cursor_y = input->cursor_pos_y;
    // Check if user is hovering over any buttons
    bool hovered_over_button = 0;

    for (uint32_t i = 0; i < widget_count; ++i) {
        bool hovered_over = ui_hover_over_box(&widgets[i].box, cursor_x, cursor_y);

        color_pair_t pair = widgets[i].color.update(
            MENU_WIDGET_HOVER_COLOR_FADE_SPEED,
            hovered_over);
if (hovered_over) {current_button = i;
            hovered_over_button = 1;
            widgets[i].hovered_on = 1;
        }
        else {
            widgets[i].hovered_on = 0;
        }

        widgets[i].image_box.color = pair.current_foreground;
        widgets[i].box.color = pair.current_background;
    }

    if (!hovered_over_button) {
        current_button = widget_count;
    }

    if (input->buttons[BT_MOUSE_LEFT].instant && current_button != widget_count) {
        menu_click_handler_t proc = procs[current_button];
        if (proc) {
            proc(events);
        }
        else {
            open_menu(
                current_button);
        }
    }

    // Update the open menu
    if (menu_in_out_transition && !menu_slider.in_animation) {
        menu_in_out_transition = 0;

        menu_slider.set(
            1,
            menu_slider.current,
            menu_slider_x_max_size,
            MENU_SLIDER_SPEED);

        return 0;
    }
    else {
        return menu_opened();
    }
}

bool menu_layout_t::menu_opened() {
    return current_open_menu != widget_count && !menu_slider.in_animation;
}

void menu_layout_t::open_menu(
    uint32_t button) {
    if (button != current_open_menu) {
        if (current_open_menu == widget_count) {
            current_open_menu = button;

            menu_slider.set(
                1,
                menu_slider.current,
                menu_slider_x_max_size,
                MENU_SLIDER_SPEED);
        }
        else {
            current_open_menu = button;

            menu_slider.set(
                1,
                menu_slider.current,
                0.0f,
                MENU_SLIDER_SPEED);

            menu_in_out_transition = 1;
        }
    }
    else {
        current_open_menu = widget_count;

        menu_slider.set(
            1,
            menu_slider.current,
            0.0f,
            MENU_SLIDER_SPEED);
    }
}