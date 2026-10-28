ENGINE = dc_engine dc_input dc_camera dc_model dc_draw2d dc_draw dc_debug dc_corona dc_scene
GENERATED_GLBS = $(addprefix assets/city/,tower0.glb tower1.glb tower2.glb tower3.glb tower4.glb tower5.glb street.glb)
include ../../mk/example.mk

$(GENERATED_GLBS) &: generate_city.py
	python3 generate_city.py

# Keep the player's jet uncompressed; other assets retain the default VQ.
$(PC)/jet/jet.dms: STRIPPY_FLAGS += -no-vq
