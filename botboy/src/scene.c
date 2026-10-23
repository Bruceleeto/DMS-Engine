#include "scene.h"
#include <stddef.h>

static const SceneFuncs* scenes[SCENE_COUNT];
static Scene current;
static Scene next;
static bool  pending;

static void fill(void) {
    scenes[SCENE_SPLASH] = &splash_scene;
    scenes[SCENE_LOGO]   = &logo_scene;
    scenes[SCENE_MENU]   = &menu_scene;
    scenes[SCENE_GAME]   = &game_scene;
}

void scene_change(Scene scene) {
    next = scene;
    pending = true;
}

void scene_start(Scene first) {
    fill();
    current = first;
    scenes[current]->init();
}

void scene_update(float dt) {
    scenes[current]->update(dt);
    if (pending) {
        pending = false;
        scenes[current]->deinit();
        current = next;
        scenes[current]->init();
    }
}

void scene_draw(void) {
    scenes[current]->draw();
}
