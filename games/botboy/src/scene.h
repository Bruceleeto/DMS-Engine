#ifndef BOTBOY_SCENE_H
#define BOTBOY_SCENE_H

typedef enum {
    SCENE_SPLASH,
    SCENE_LOGO,
    SCENE_MENU,
    SCENE_GAME,
    SCENE_COUNT
} Scene;

typedef struct {
    void (*init)(void);
    void (*deinit)(void);
    void (*update)(float dt);
    void (*draw)(void);
} SceneFuncs;

extern const SceneFuncs splash_scene, logo_scene, menu_scene, game_scene;

/* Asks for a scene; it changes after this frame's update */
void scene_change(Scene scene);

/* Engine of the scenes: called by main */
void scene_start(Scene first);
void scene_update(float dt);
void scene_draw(void);

#endif
