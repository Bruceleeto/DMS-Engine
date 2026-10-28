/* dc_scene: a scene put together in the editor (or by hand), as a JSON file.
 *
 * A scene is the models placed in it, the lights, the background colour,
 * fog and where the camera starts. The game loads it and draws it:
 *
 *     DCScene* scene = dc_scene_play(ASSETS, "scenes/main.json");
 *     dc_scene_apply(scene);              // background, fog, lights
 *     dc_scene_camera(scene, &camera);    // where the camera starts
 *     ...each frame
 *     dc_set_camera(&camera);
 *     dc_scene_draw(scene);
 *
 * Model paths in the file are under root (normally ASSETS), so
 * "teapot/teapot.dms" is assets/teapot/teapot.glb converted; "@cube" is the
 * engine's white cube (dc_model_cube), 1 unit across. Objects using
 * the same model share one copy of it. dc_scene_find() gets an object by
 * name, to move it from game code.
 *
 * An object with no model is a marker: a named place, as a Blender Empty
 * is, that the game reads and nothing draws. Markers are the Empties of a
 * model moved into the scene, where the editor shows them and they can be
 * moved (dc_scene_import_entities), and dc_scene_entities() hands them back
 * to the game as that model's Empties, so dc_model_entity() finds them.
 * A marker's scale is its size in the world: the Empty's times the model's.
 *
 * The file, every field optional:
 *
 *     { "dms_scene": 1,
 *       "background": "#101820",
 *       "fog": { "color": "#808890", "near": 50, "far": 400 },
 *       "camera": { "pos": [0, 2, -6], "yaw": 0, "pitch": 10, "fov": 60 },
 *       "lights": [ { "name": "Sun", "pos": [-0.4, -1, -0.3], "sun": true,
 *                     "color": "#ffffff", "range": 500, "ambient": 0.25, "bands": 0 } ],
 *       "objects": [ { "name": "Teapot", "model": "teapot/teapot.dms",
 *                      "pos": [0, 0, 0], "rot": [0, 45, 0], "scale": [1, 1, 1],
 *                      "tint": "#ffffff", "add": false, "visible": true } ] }
 *
 * Angles are degrees. Camera yaw and pitch are DCCamera's (yaw 0 looks along
 * +z, positive pitch looks down). An object's rot turns it about x, y and z
 * of the world, z first, then x, then y, right-handed. A skinned model only
 * takes the turn about y. A sun's pos is the way its light goes, as DCLight's. */
#ifndef DC_SCENE_H
#define DC_SCENE_H

#include "dc_camera.h"
#include "dc_model.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char       name[32];
    char       model[96];   /* under the scene's root */
    shz_vec3_t pos;
    shz_vec3_t rot;         /* degrees about x, y, z */
    shz_vec3_t scale;
    uint32_t   tint;        /* 0xRRGGBB; 0xFFFFFF (or 0) is as it is */
    bool       add;         /* added to what is behind it (glows) */
    bool       hidden;
    DMSModel*  m;           /* the loaded model, NULL if it could not be */
} DCSceneObject;

typedef struct {
    char    name[32];
    DCLight light;
} DCSceneLight;

typedef struct {
    char        path[96];
    DMSModel*   model;
} DCSceneModel;

typedef struct {
    char       root[128];
    uint32_t   background;  /* 0xRRGGBB */
    bool       fog;
    uint32_t   fog_rgb;
    float      fog_near, fog_far;
    shz_vec3_t cam_pos;
    float      cam_yaw, cam_pitch, cam_fov;   /* degrees */

    DCSceneObject* objects;
    int            object_count, object_cap;
    DCSceneLight   lights[DC_MAX_LIGHTS];
    int            light_count;

    DCSceneModel*  models;  /* loaded once each, shared by the objects */
    int            model_count, model_cap;
} DCScene;

/* Read a scene and load its models. NULL if the file is missing or not a
 * scene; a model that won't load leaves its object's m NULL (not drawn). */
DCScene* dc_scene_load(const char* path, const char* root);
/* The scene to play: the one the editor has open when its Play runs the
 * game (DMS_SCENE, Linux only), else the one named, both under root. */
DCScene* dc_scene_play(const char* root, const char* scene);
/* The model of the object of that name (NULL if none) */
DMSModel* dc_scene_model(DCScene* scene, const char* name);
/* The same from text already in memory. */
DCScene* dc_scene_parse(const char* json, const char* root);
/* An empty scene (the editor's File > New). */
DCScene* dc_scene_new(const char* root);
void     dc_scene_free(DCScene* scene);

/* Background, fog and lights, from now on. */
void dc_scene_apply(const DCScene* scene);
/* The camera where the scene says it starts. */
void dc_scene_camera(const DCScene* scene, DCCamera* cam);
/* Queue every visible object (call between dc_frame_begin and dc_frame_end). */
void dc_scene_draw(const DCScene* scene);

DCSceneObject* dc_scene_find(DCScene* scene, const char* name);
/* Where an object's x, y and z axes point (3 columns, unit length, without
 * its scale), as DCDrawOpts.rot takes them. */
void dc_scene_object_rot(const DCSceneObject* obj, float rot[9]);

/* Editing: add an object (model loaded now), remove one, and load the
 * models of objects whose m is NULL (after changing an object's model, set
 * its m to NULL and call this). */
DCSceneObject* dc_scene_add(DCScene* scene, const char* name, const char* model);
void dc_scene_remove(DCScene* scene, int index);
void dc_scene_load_models(DCScene* scene);

/* Markers: an object with no model (model[0] == 0) */
static inline bool dc_scene_is_marker(const DCSceneObject* obj) { return !obj->model[0]; }
/* The Empties of host's model added as markers, placed in the world as host
 * places the model; how many. Done once, in the editor; then the scene has
 * them and the model's own are not read. */
int dc_scene_import_entities(DCScene* scene, const DCSceneObject* host);
/* The scene's markers as host's model's Empties, in its own space (host's
 * place and scale undone; its turn is not), in place of those in the file.
 * How many; a scene with no markers leaves the model's as they are. */
int dc_scene_entities(const DCScene* scene, DCSceneObject* host);
/* Degrees about x, y, z from 3 axis columns: dc_scene_object_rot backwards */
shz_vec3_t dc_scene_rot_degrees(const float rot[9]);

/* The scene as JSON text, malloc'd (the caller frees it). */
char* dc_scene_write(const DCScene* scene);

#endif
