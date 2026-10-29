# shooter

A first person shooter: Half-Life 2's SMG in your hands on Civilization
Defense, a Garry's Mod workshop map (3468839489), and Left 4 Dead's infected
coming at you in waves. B or the R trigger fires (left Ctrl or the left mouse
button on a PC), Y reloads (R), A jumps (Space) and goes again once you are
dead, the L trigger swaps to the stun gun and back (Q), X switches the torch,
Start quits. The stun gun's arc reaches 10 m and leaves one of them stunned
for a moment, its skeleton flickering through, then it drops dead. (The
skeleton is Garry's Mod's, put on the infected's bones, cut down and plain.)

Valve's assets and the map's, for trying the engine out only: not for a
release, and not in the repo. With Half-Life 2, Left 4 Dead and Garry's Mod installed
(Steam) and the map and the stun gun (workshop 2595774444, "Taser / Stungun",
optional) subscribed to in the workshop, bring them in once (the scene, scenes/main.json, is already here):

    make -C tools/gmod
    G=games/shooter/assets
    tools/gmod/gmod2glb maps/civilization_defensevmf.bsp --assets $G && rm $G/scenes/civilization_defensevmf.json
    mkdir -p $G/gmod/smg1
    tools/gmod/gmod2glb models/weapons/v_smg1.mdl -o $G/gmod/smg1/smg1.glb --tex 256 --anims @idle01,@fire01,@draw,@reload
    mkdir -p $G/gmod/infected
    tools/gmod/gmod2glb models/infected/common_male01.mdl -o $G/gmod/infected/infected.glb --tex 256 \
        --merge models/player/skeleton.mdl --merge-keep 0.15 --merge-tex -1 \
        --anims a_Run_01,@Melee_03,@Idle_Neutral_01,@Death_03,@Death_06,@Death_09,@Death_10ab
    mkdir -p $G/gmod/taser
    tools/gmod/gmod2glb models/realistic_police/taser/c_taser.mdl -o $G/gmod/taser/taser.glb --tex 256 \
        --anims all --merge models/weapons/c_arms_hev.mdl

See the [top-level README](../../README.md) for building.
