# RBO GameCube prototype — slim DOL + SD-loaded TPLs / SOUND
#
#   make          → rbo_fabre_proto.dol (code only, no embedded TPL)
#   make sdpack   → sd_pack/RBO/RBO.DOL + ASSETS/*.TPL
#   make gcm      → sd_pack/RBO.gcm (same RBO folder in a disc image)
#   tools/export-sound-sd.ps1 → sd_pack/RBO/SOUND/bgm/*.ogg + se/*.wav
#
# Copy sd_pack/RBO onto the SD2SP2 card (sdc:/RBO/...).
# Open sd_pack/RBO.gcm in Dolphin to boot from disc (no Slot A SD needed).

.SUFFIXES:
.SECONDARY:
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/gamecube_rules

TARGET		:=	rbo_fabre_proto
BUILD		:=	build
SOURCES		:=	source
DATA		:=
TEXTURES	:=	textures
INCLUDES	:=

CFLAGS	:=	-g -O2 -Wall $(MACHDEP) $(INCLUDE)
CXXFLAGS	:=	$(CFLAGS)
LDFLAGS	:=	-g $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS	:=	-lvorbisidec -logg -lasnd -lfat -logc -lm
LIBDIRS	:=	$(PORTLIBS)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
					$(foreach dir,$(TEXTURES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))
SCFFILES	:=	textures.scf
TPLFILES	:=	$(SCFFILES:.scf=.tpl)

ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

# Do NOT embed TPL via bin2o — SD loader owns texture bytes.
export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(sFILES:.s=.o) $(SFILES:.S=.o)
export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)
export HFILES := $(addsuffix .h,$(subst .,_,$(BINFILES))) textures.h

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD) \
					-I$(LIBOGC_INC)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					-L$(LIBOGC_LIB)

.PHONY: $(BUILD) clean sdpack gcm wholesale

PYTHON		?=	python
APPLDR_ELF	:=	$(CURDIR)/tools/appldr/appldr.elf
APPLDR_BIN	:=	$(CURDIR)/tools/appldr/appldr.bin

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C "$(BUILD)" -f "$(CURDIR)/Makefile"

# Boot pack: caution / Boot_logo atlas / intro still / title atlas.
$(BUILD)/boot.tpl: textures/boot.scf textures/cscreen.png textures/boot_logo_atlas.png textures/intro_screen.png textures/title_atlas.png textures/gc_pad_icons.png
	@echo boot.scf
	@cd "$(BUILD)" && gxtexconv -s ../textures/boot.scf -o boot.tpl

$(BUILD)/lobby.tpl: textures/lobby.scf textures/lobby_atlas.png textures/create_screen.png textures/gc_pad_icons.png
	@echo lobby.scf
	@cd "$(BUILD)" && gxtexconv -s ../textures/lobby.scf -o lobby.tpl

$(BUILD)/stsel.tpl: textures/stsel.scf textures/stsel_atlas.png textures/stsel_chip01.png textures/gc_pad_icons.png
	@echo stsel.scf
	@cd "$(BUILD)" && gxtexconv -s ../textures/stsel.scf -o stsel.tpl

sdpack: $(BUILD) $(BUILD)/boot.tpl $(BUILD)/lobby.tpl $(BUILD)/stsel.tpl
	@mkdir -p sd_pack/RBO/ASSETS
	@mkdir -p sd_pack/RBO/SCRIPTS
	@cp -f "$(OUTPUT).dol" sd_pack/RBO/RBO.DOL
	@cp -f "$(CURDIR)/$(BUILD)/textures.tpl" sd_pack/RBO/ASSETS/STAGE01.TPL
	@cp -f "$(CURDIR)/$(BUILD)/boot.tpl" sd_pack/RBO/ASSETS/TITLE.TPL
	@cp -f "$(CURDIR)/$(BUILD)/lobby.tpl" sd_pack/RBO/ASSETS/LOBBY.TPL
	@cp -f "$(CURDIR)/$(BUILD)/stsel.tpl" sd_pack/RBO/ASSETS/STSEL.TPL
	@cp -f "$(CURDIR)/assets/scripts/STAGE01.FOB" sd_pack/RBO/SCRIPTS/STAGE01.FOB
	@echo "sd_pack ready:"
	@ls -la sd_pack/RBO/RBO.DOL sd_pack/RBO/ASSETS/*.TPL sd_pack/RBO/SCRIPTS/STAGE01.FOB
	@if [ -d sd_pack/RBO/SOUND ]; then ls -la sd_pack/RBO/SOUND/bgm sd_pack/RBO/SOUND/se; \
	 else echo "NOTE: SOUND/ missing — run tools/export-sound-sd.ps1"; fi

$(APPLDR_BIN): tools/appldr/appldr.c tools/appldr/appldr.ld
	@echo appldr
	@$(CC) -O2 -G0 -msdata=none -ffreestanding -fno-asynchronous-unwind-tables \
		-DGEKKO -mcpu=750 -meabi -mhard-float \
		-nostartfiles -nostdlib -nodefaultlibs \
		-Wl,-T,$(CURDIR)/tools/appldr/appldr.ld \
		-o $(APPLDR_ELF) $(CURDIR)/tools/appldr/appldr.c
	@$(OBJCOPY) -O binary $(APPLDR_ELF) $@

gcm: sdpack $(APPLDR_BIN)
	@$(PYTHON) $(CURDIR)/tools/pack-gcm.py \
		--root $(CURDIR)/sd_pack/RBO \
		--dol $(CURDIR)/sd_pack/RBO/RBO.DOL \
		--apploader $(APPLDR_BIN) \
		--out $(CURDIR)/sd_pack/RBO.gcm

# Second DOL: Win32/DirectX host. Does not replace RBO.DOL.
wholesale:
	@$(MAKE) -C "$(CURDIR)/wholesale"
	@mkdir -p sd_pack/RBO
	@cp -f "$(CURDIR)/wholesale/rbo_ex3.dol" sd_pack/RBO/RBO_EX3.DOL
	@ls -la sd_pack/RBO/RBO_EX3.DOL

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol sd_pack $(APPLDR_ELF) $(APPLDR_BIN)

else

# Build TPL for SD pack / textures.h, but do NOT pass it to the linker.
$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES) | textures.tpl
$(OFILES_SOURCES): textures.h

textures.h: textures.tpl
	@test -f textures.h

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

endif
