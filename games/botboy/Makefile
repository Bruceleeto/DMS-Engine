SRCS = $(addprefix src/,$(addsuffix .c,main sound scene splash logo menu ui save game robot props enemies screens hud fx demo))
ENGINE_EXTRA = dc_particles dc_audio

# make DEV=1: the dev keys in a level, Y for god mode and X+Y to record a demo
ifeq ($(DEV),1)
DEFINES += -DBOTBOY_DEV
endif

include ../../mk/example.mk
