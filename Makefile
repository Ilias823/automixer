# Automixer: build, test, install.
BUILD   := build
CONFIG  := Release
VST3    := $(BUILD)/Automixer_artefacts/$(CONFIG)/VST3/Automixer.vst3
DEST    := $(HOME)/Library/Audio/Plug-Ins/VST3/Automixer.vst3
BIN     := Contents/MacOS/Automixer

.PHONY: all configure build test install uninstall clean

all: build

$(BUILD)/CMakeCache.txt:
	cmake -B $(BUILD) -G Xcode

configure: $(BUILD)/CMakeCache.txt

build: configure
	cmake --build $(BUILD) --config $(CONFIG) --target Automixer_VST3 automix_cli am_tests host_check
	codesign --force --deep --sign - $(VST3)

test: build
	$(BUILD)/$(CONFIG)/am_tests
	$(BUILD)/host_check_artefacts/$(CONFIG)/host_check $(VST3)

# Installs for the current user and verifies the installed binary matches the build.
install: build
	rm -rf "$(DEST)"
	mkdir -p "$(dir $(DEST))"
	cp -R $(VST3) "$(DEST)"
	cmp "$(VST3)/$(BIN)" "$(DEST)/$(BIN)" && echo "Installed and verified: $(DEST)"
	@echo "Restart DaVinci Resolve to load the new build."

uninstall:
	rm -rf "$(DEST)"

clean:
	rm -rf $(BUILD)
