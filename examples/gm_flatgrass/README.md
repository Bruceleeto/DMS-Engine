# gm_flatgrass

Garry's Mod's gm_flatgrass with Alyx and a headcrab from Half-Life 2
wandering about: each walks to a spot near where the scene puts it, stands a
while, then goes somewhere else. A free camera flies through it (Y goes back
to the start, Start quits).

Valve's and Facepunch's assets, for trying the engine out only: not for a
release, and not in the repo. With Garry's Mod and Half-Life 2 installed
(Steam), bring them in once (the scene, scenes/main.json, is already here):

    make -C tools/gmod
    G=examples/gm_flatgrass/assets
    tools/gmod/gmod2glb maps/gm_flatgrass.bsp --assets $G && rm $G/scenes/gm_flatgrass.json
    mkdir -p $G/gmod/alyx $G/gmod/headcrab
    tools/gmod/gmod2glb models/alyx.mdl -o $G/gmod/alyx/alyx.glb --tex 256 --anims a_WalkN,@LineIdle01
    tools/gmod/gmod2glb models/headcrabclassic.mdl -o $G/gmod/headcrab/headcrab.glb --tex 256 --anims @Run1,@Idle01

See the [top-level README](../../README.md) for building.
