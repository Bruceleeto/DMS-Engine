# gm_construct

Free camera through Garry's Mod's gm_construct: the map as one model with its
baked lighting in the vertex colours, and its props where the map places them.
Brought in with the editor's Garry's Mod tab (Maps, gm_construct, Bring in).

Valve's and Facepunch's assets, for trying the engine out only: not for a
release, and not in the repo. With Garry's Mod installed (Steam), bring them
in once (the scene, scenes/main.json, is already here):

    make -C tools/gmod
    tools/gmod/gmod2glb maps/gm_construct.bsp --assets examples/gm_construct/assets --simplify 0.2
    rm examples/gm_construct/assets/scenes/gm_construct.json

See the [top-level README](../../README.md) for building.
