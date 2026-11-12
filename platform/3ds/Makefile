# armGBA-3DS: make (or make JIT=0 for the interpreter alone) builds armGBA-3DS.3dsx; make cia
# also builds armGBA-3DS.cia (needs makerom and bannertool on the PATH).
#
# LOG_URL=http://<host>:<port>/log: a development build that sends its logs there at start
# (source/logupload.c), and logs each second's performance whatever the bottom screen shows. Not
# for releases.
#
# Needs devkitARM with libctru, citro3d, citro2d and zlib (devkitpro/devkitarm's Docker image has
# them all): see README-3DS.md.

ifeq ($(strip $(DEVKITARM)),)
$(error "Set DEVKITARM in the environment: export DEVKITARM=<devkitPro>/devkitARM")
endif

include $(DEVKITARM)/3ds_rules

TARGET   := armGBA-3DS
ROOT     := ../..
BUILD    := build
JIT      ?= 1
# The release this is or leads up to; a build that isn't of the release's tag (3ds-v0.0.2-alpha)
# says which commit it is of.
RELEASE  := 0.0.2-alpha
MAJOR    := 0
MINOR    := 0
MICRO    := 2
COMMIT   := $(shell git -C $(ROOT) rev-parse --short HEAD 2>/dev/null)
TAGGED   := $(shell git -C $(ROOT) describe --tags --exact-match 2>/dev/null)
VERSION  ?= $(RELEASE)$(if $(filter 3ds-v$(RELEASE),$(TAGGED)),,$(if $(COMMIT),+$(COMMIT)))

APP_TITLE       := armGBA-3DS
APP_DESCRIPTION := Game Boy Advance, Game Boy and Game Boy Color
APP_AUTHOR      := Magnominus; 3DS port: ratherDashing

ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
# -fwrapv: the core keeps its clocks in 32-bit counters that wrap after about 128 seconds and compares
# them by subtracting, which signed overflow must allow. devkitARM's GCC otherwise optimizes those
# comparisons away and the console hangs 128 seconds into a game.
# -flto: the core's hot paths cross its files (bus, timing, memory, video); compiled as one program,
# an Advance Wars scene with an interrupt every line took 8% less time and a Mario Kart race 2.5%
# (ARM build of the core under qemu, 2026-11-09).
COMMON   := -g -Wall -fwrapv -flto -fomit-frame-pointer -ffunction-sections -mword-relocations $(ARCH) \
            -D__3DS__ -I$(CTRULIB)/include -I$(PORTLIBS)/include
CFLAGS   := $(COMMON) -std=gnu11
CXXFLAGS := $(COMMON) -std=gnu++11 -fno-exceptions -fno-rtti
ASFLAGS  := -g $(ARCH)
LDFLAGS  := -specs=3dsx.specs -g -O3 -fwrapv -flto=auto -mword-relocations $(ARCH) -Wl,-Map,$(BUILD)/$(TARGET).map -Wl,--gc-sections
LIBS     := -L$(CTRULIB)/lib -L$(PORTLIBS)/lib -lcitro2d -lcitro3d -lctru -lz -lm

# The core, as CMakeLists.txt builds it, with the A32 recompiler (ARMv6 code: src/cpu/a32/emit.h)
CORE := bios-image.c bus.c cheats.c gba.c memory.c state.c timing.c \
        audio/audio.c audio/audio-thread.c audio/psg.c cart/cart.c cart/ereader.c cart/gpio.c cart/overrides.c cart/savedata.c \
        cpu/arm.c cpu/decode.c cpu/step.c io/dma.c io/io.c io/rfu.c io/sio.c io/timer.c \
        video/render-thread.c video/render.c video/video.c
ifeq ($(JIT),1)
CORE += cpu/jit.c cpu/a32/backend.c
else
CORE += cpu/nojit.c
endif
CORE_SRC := $(addprefix $(ROOT)/src/,$(CORE))

LIBRETRO_SRC := $(addprefix $(ROOT)/libretro/,libretro.c clock.c lcd.c options.c saves.c dolphin.c link.c games.c \
                savefiles.c patch.c dmg07.c ereader.c printer.c printout.c workers.c gb-gambatte.cpp)
GAMBATTE_SRC := $(filter-out %/file_zip.cpp %/cinterface.cpp,$(shell find $(ROOT)/gambatte/libgambatte/src -name '*.cpp' -not -path '*/unzip/*'))
ARCHIVE_SRC  := $(shell find $(ROOT)/libretro/deps/libretro-common $(ROOT)/libretro/deps/7zip -name '*.c')
FRONTEND_SRC := $(wildcard source/*.c) $(wildcard source/*.s)

objs = $(patsubst $(ROOT)/%,$(BUILD)/%.o,$(patsubst source/%,$(BUILD)/frontend/%.o,$(1)))
CORE_OBJ     := $(call objs,$(CORE_SRC))
LIBRETRO_OBJ := $(call objs,$(LIBRETRO_SRC))
GAMBATTE_OBJ := $(call objs,$(GAMBATTE_SRC))
ARCHIVE_OBJ  := $(call objs,$(ARCHIVE_SRC))
FRONTEND_OBJ := $(call objs,$(FRONTEND_SRC))
OBJ := $(CORE_OBJ) $(LIBRETRO_OBJ) $(GAMBATTE_OBJ) $(ARCHIVE_OBJ) $(FRONTEND_OBJ)

ARCHIVE_INC := -I$(ROOT)/libretro/deps/libretro-common/include -I$(ROOT)/libretro/deps
GAMBATTE_INC := -I$(ROOT)/gambatte/libgambatte/include -I$(ROOT)/gambatte/libgambatte/src -I$(ROOT)/gambatte/common -DHAVE_STDINT_H

$(CORE_OBJ):     EXTRA := -O3 -Wextra -Wno-unused-parameter -I$(ROOT)/src
$(LIBRETRO_OBJ): EXTRA := -O2 -I$(ROOT)/src -I$(ROOT)/libretro/deps/libretro-common/include $(GAMBATTE_INC)
$(GAMBATTE_OBJ): EXTRA := -O2 -w $(GAMBATTE_INC)
$(ARCHIVE_OBJ):  EXTRA := -O2 -w $(ARCHIVE_INC) -DHAVE_ZLIB -DHAVE_7ZIP
$(FRONTEND_OBJ): EXTRA := -O2 -Wextra -Wno-unused-parameter -I$(ROOT)/libretro -I$(ROOT)/src $(ARCHIVE_INC) \
                          -DARMGBA_3DS_VERSION='"$(VERSION)"' $(if $(LOG_URL),-DARMGBA_LOG_URL='"$(LOG_URL)"')

.PHONY: all clean cia
all: $(TARGET).3dsx

$(BUILD)/%.c.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	@echo $(notdir $<)
	@$(CC) -MMD -MP $(CFLAGS) $(EXTRA) -c $< -o $@

$(BUILD)/%.cpp.o: $(ROOT)/%.cpp
	@mkdir -p $(dir $@)
	@echo $(notdir $<)
	@$(CXX) -MMD -MP $(CXXFLAGS) $(EXTRA) -c $< -o $@

$(BUILD)/frontend/%.c.o: source/%.c
	@mkdir -p $(dir $@)
	@echo $(notdir $<)
	@$(CC) -MMD -MP $(CFLAGS) $(EXTRA) -c $< -o $@

$(BUILD)/frontend/%.s.o: source/%.s
	@mkdir -p $(dir $@)
	@echo $(notdir $<)
	@$(CC) -x assembler-with-cpp $(ASFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJ)
	@echo linking $@
	@$(CXX) $(LDFLAGS) $(OBJ) $(LIBS) -o $@

$(TARGET).smdh: icon.png
	@smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" icon.png $@

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
	@3dsxtool $< $@ --smdh=$(TARGET).smdh
	@echo built $@ $(VERSION)

# The CIA: an icon and banner from bannertool, packed by makerom with the RSF's permissions.
# icon.png, banner.png and banner.wav are drawn by art/make.py.
$(BUILD)/banner.bnr: banner.png banner.wav
	@bannertool makebanner -i banner.png -a banner.wav -o $@
$(BUILD)/icon.icn: icon.png
	@bannertool makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i icon.png -o $@
$(TARGET).cia: $(TARGET).elf $(BUILD)/banner.bnr $(BUILD)/icon.icn armgba.rsf
	@makerom -f cia -o $@ -elf $(TARGET).elf -rsf armgba.rsf -target t -exefslogo -icon $(BUILD)/icon.icn \
	         -banner $(BUILD)/banner.bnr -major $(MAJOR) -minor $(MINOR) -micro $(MICRO)
	@echo built $@
cia: $(TARGET).cia

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).3dsx $(TARGET).smdh $(TARGET).cia

-include $(OBJ:.o=.d)
