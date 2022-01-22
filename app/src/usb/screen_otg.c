#include "screen_otg.h"

#include "icon.h"
#include "options.h"
#include "util/log.h"

static void
sc_screen_otg_capture_mouse(struct sc_screen_otg *screen, bool capture) {
    if (SDL_SetRelativeMouseMode(capture)) {
        LOGE("Could not set relative mouse mode to %s: %s",
             capture ? "true" : "false", SDL_GetError());
        return;
    }

    screen->mouse_captured = capture;
}

static void
sc_screen_otg_render(struct sc_screen_otg *screen) {
    SDL_RenderClear(screen->renderer);
    if (screen->texture) {
        SDL_RenderCopy(screen->renderer, screen->texture, NULL, NULL);
    }
    SDL_RenderPresent(screen->renderer);
}

bool
sc_screen_otg_init(struct sc_screen_otg *screen,
                   const struct sc_screen_otg_params *params) {
    screen->keyboard = params->keyboard;
    screen->mouse = params->mouse;

    screen->mouse_captured = false;
    screen->mouse_capture_key_pressed = 0;

    const char *title = params->window_title;
    assert(title);

    int x = params->window_x != SC_WINDOW_POSITION_UNDEFINED
          ? params->window_x : (int) SDL_WINDOWPOS_UNDEFINED;
    int y = params->window_y != SC_WINDOW_POSITION_UNDEFINED
          ? params->window_y : (int) SDL_WINDOWPOS_UNDEFINED;
    int width = 256;
    int height = 256;

    uint32_t window_flags = 0;
    if (params->always_on_top) {
        window_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    }
    if (params->window_borderless) {
        window_flags |= SDL_WINDOW_BORDERLESS;
    }

    screen->window = SDL_CreateWindow(title, x, y, width, height, window_flags);
    if (!screen->window) {
        LOGE("Could not create window: %s", SDL_GetError());
        return false;
    }

    screen->renderer = SDL_CreateRenderer(screen->window, -1, 0);
    if (!screen->renderer) {
        LOGE("Could not create renderer: %s", SDL_GetError());
        goto error_destroy_window;
    }

    SDL_Surface *icon = scrcpy_icon_load();

    if (icon) {
        SDL_SetWindowIcon(screen->window, icon);

        screen->texture = SDL_CreateTextureFromSurface(screen->renderer, icon);
        scrcpy_icon_destroy(icon);
        if (!screen->texture) {
            goto error_destroy_renderer;
        }
    } else {
        screen->texture = NULL;
        LOGW("Could not load icon");
    }

    // Capture mouse on start
    sc_screen_otg_capture_mouse(screen, true);

    return true;

error_destroy_window:
    SDL_DestroyWindow(screen->window);
error_destroy_renderer:
    SDL_DestroyRenderer(screen->renderer);

    return false;
}

void
sc_screen_otg_destroy(struct sc_screen_otg *screen) {
    if (screen->texture) {
        SDL_DestroyTexture(screen->texture);
    }
    SDL_DestroyRenderer(screen->renderer);
    SDL_DestroyWindow(screen->window);
}

static inline bool
sc_screen_otg_is_mouse_capture_key(SDL_Keycode key) {
    return key == SDLK_LALT || key == SDLK_LGUI || key == SDLK_RGUI;
}

static void
sc_screen_otg_process_key(struct sc_screen_otg *screen,
                             const SDL_KeyboardEvent *event) {
    assert(screen->keyboard);
    struct sc_key_processor *kp = &screen->keyboard->key_processor;

    struct sc_key_event evt = {
        .action = sc_action_from_sdl_keyboard_type(event->type),
        .keycode = sc_keycode_from_sdl(event->keysym.sym),
        .scancode = sc_scancode_from_sdl(event->keysym.scancode),
        .repeat = event->repeat,
        .mods_state = sc_mods_state_from_sdl(event->keysym.mod),
    };

    assert(kp->ops->process_key);
    kp->ops->process_key(kp, &evt, SC_SEQUENCE_INVALID);
}

static void
sc_screen_otg_process_mouse_motion(struct sc_screen_otg *screen,
                                   const SDL_MouseMotionEvent *event) {
    assert(screen->mouse);
    struct sc_mouse_processor *mp = &screen->mouse->mouse_processor;

    struct sc_mouse_motion_event evt = {
        // .position not used for HID events
        .xrel = event->xrel,
        .yrel = event->yrel,
        .buttons_state = sc_mouse_buttons_state_from_sdl(event->state, true),
    };

    assert(mp->ops->process_mouse_motion);
    mp->ops->process_mouse_motion(mp, &evt);
}

static void
sc_screen_otg_process_mouse_button(struct sc_screen_otg *screen,
                                   const SDL_MouseButtonEvent *event) {
    assert(screen->mouse);
    struct sc_mouse_processor *mp = &screen->mouse->mouse_processor;

    uint32_t sdl_buttons_state = SDL_GetMouseState(NULL, NULL);

    struct sc_mouse_click_event evt = {
        // .position not used for HID events
        .action = sc_action_from_sdl_mousebutton_type(event->type),
        .button = sc_mouse_button_from_sdl(event->button),
        .buttons_state =
            sc_mouse_buttons_state_from_sdl(sdl_buttons_state, true),
    };

    assert(mp->ops->process_mouse_click);
    mp->ops->process_mouse_click(mp, &evt);
}

static void
sc_screen_otg_process_mouse_wheel(struct sc_screen_otg *screen,
                                  const SDL_MouseWheelEvent *event) {
    assert(screen->mouse);
    struct sc_mouse_processor *mp = &screen->mouse->mouse_processor;

    uint32_t sdl_buttons_state = SDL_GetMouseState(NULL, NULL);

    struct sc_mouse_scroll_event evt = {
        // .position not used for HID events
        .hscroll = event->x,
        .vscroll = event->y,
        .buttons_state =
            sc_mouse_buttons_state_from_sdl(sdl_buttons_state, true),
    };

    assert(mp->ops->process_mouse_scroll);
    mp->ops->process_mouse_scroll(mp, &evt);
}

void
sc_screen_otg_handle_event(struct sc_screen_otg *screen, SDL_Event *event) {
    switch (event->type) {
        case SDL_WINDOWEVENT:
            switch (event->window.event) {
                case SDL_WINDOWEVENT_EXPOSED:
                    sc_screen_otg_render(screen);
                    break;
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    sc_screen_otg_capture_mouse(screen, false);
                    break;
            }
            return;
        case SDL_KEYDOWN: {
            SDL_Keycode key = event->key.keysym.sym;
            if (sc_screen_otg_is_mouse_capture_key(key)) {
                if (!screen->mouse_capture_key_pressed) {
                    screen->mouse_capture_key_pressed = key;
                } else {
                    // Another mouse capture key has been pressed, cancel mouse
                    // (un)capture
                    screen->mouse_capture_key_pressed = 0;
                }
                // Mouse capture keys are never forwarded to the device
                return;
            }

            sc_screen_otg_process_key(screen, &event->key);
            break;
        }
        case SDL_KEYUP: {
            SDL_Keycode key = event->key.keysym.sym;
            SDL_Keycode cap = screen->mouse_capture_key_pressed;
            screen->mouse_capture_key_pressed = 0;
            if (sc_screen_otg_is_mouse_capture_key(key)) {
                if (key == cap) {
                    // A mouse capture key has been pressed then released:
                    // toggle the capture mouse mode
                    sc_screen_otg_capture_mouse(screen,
                                                !screen->mouse_captured);
                }
                // Mouse capture keys are never forwarded to the device
                return;
            }

            sc_screen_otg_process_key(screen, &event->key);
            break;
        }
        case SDL_MOUSEMOTION:
            if (screen->mouse_captured) {
                sc_screen_otg_process_mouse_motion(screen, &event->motion);
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (screen->mouse_captured) {
                sc_screen_otg_process_mouse_button(screen, &event->button);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (screen->mouse_captured) {
                sc_screen_otg_process_mouse_button(screen, &event->button);
            } else {
                sc_screen_otg_capture_mouse(screen, true);
            }
            break;
        case SDL_MOUSEWHEEL:
            if (screen->mouse_captured) {
                sc_screen_otg_process_mouse_wheel(screen, &event->wheel);
            }
            break;
    }
}
