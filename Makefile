# Quill -- Vulkan editor on mu_quill. Tree-sitter only; no LSP/completion.
MYLIB   ?= ../mylib
CC      ?= cc
PKGCONFIG ?= pkg-config
BUILD := build

# GLFW is vendored as a git submodule (third_party/glfw, pinned to 3.4) and can be
# linked statically.  Which display backends get compiled in is a build flag, so a
# Wayland session never carries X11 code and vice versa:
#
#   make                      # static GLFW, Wayland only  (default)
#   make GLFW_PLATFORMS=x11   # static GLFW, X11 only
#   make GLFW_PLATFORMS=both  # static GLFW, Wayland + X11, picks at runtime
#   make GLFW=system          # link the distro's libglfw.so instead (A/B)
#
# Each platform choice uses its own build directory, so switching does not leave a
# stale library behind -- all three can coexist in build/ without a `make clean`.
GLFW      ?= static
GLFW_PLATFORMS ?= wayland
GLFW_DIR  := third_party/glfw
GLFW_BUILD := $(BUILD)/glfw-$(if $(filter static,$(GLFW)),$(GLFW_PLATFORMS),system)
GLFW_LIB  := $(GLFW_BUILD)/src/libglfw3.a
GLFW_PRUNE_STAMP := $(GLFW_DIR)/.pruned

ifeq ($(GLFW_PLATFORMS),wayland)
  GLFW_WAYLAND := ON
  GLFW_X11     := OFF
  GLFW_PC      := wayland-client wayland-cursor wayland-egl xkbcommon
else ifeq ($(GLFW_PLATFORMS),x11)
  GLFW_WAYLAND := OFF
  GLFW_X11     := ON
  GLFW_PC      := x11 xrandr xinerama xcursor xi
else ifeq ($(GLFW_PLATFORMS),both)
  GLFW_WAYLAND := ON
  GLFW_X11     := ON
  GLFW_PC      := wayland-client wayland-cursor wayland-egl xkbcommon x11 xrandr xinerama xcursor xi
else
  $(error GLFW_PLATFORMS must be one of: wayland, x11, both (got '$(GLFW_PLATFORMS)'))
endif

GLFW_CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=Release \
                    -DGLFW_BUILD_WAYLAND=$(GLFW_WAYLAND) -DGLFW_BUILD_X11=$(GLFW_X11) \
                    -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF \
                    -DGLFW_BUILD_DOCS=OFF -DGLFW_LIBRARY_TYPE=STATIC
ifeq ($(GLFW),static)
GLFW_LDLIBS := $(GLFW_LIB)
GLFW_DEPS   := $(GLFW_LIB)
GLFW_USE    := $(shell $(PKGCONFIG) --libs $(GLFW_PC))
else
GLFW_LDLIBS := $(shell $(PKGCONFIG) --libs glfw3)
GLFW_DEPS   :=
GLFW_USE    :=
endif
CFLAGS  := -std=c99 -O2 -Wall -Wextra -Werror -fno-strict-aliasing -march=native -D_GNU_SOURCE -MMD -MP
CFLAGS  += -I$(MYLIB) -Isrc -Ithird_party -Ithird_party/tree-sitter -Ithird_party/libvterm/include
CFLAGS  += -I$(GLFW_DIR)/include
CFLAGS  += $(shell $(PKGCONFIG) --cflags freetype2 harfbuzz vulkan)
LDLIBS := $(shell $(PKGCONFIG) --libs freetype2 harfbuzz vulkan) -lm
LDLIBS += $(GLFW_LDLIBS) $(GLFW_USE) -lpthread
# Vendored libvterm (MIT): VT escape-sequence parser + screen model for the
# integrated terminal. Built without -Werror (third-party, -Wall -Wpedantic clean).
VTERM_DIR    := third_party/libvterm
VTERM_CFLAGS := -std=c99 -O2 -march=native -fno-strict-aliasing -MMD -MP \
                -I$(VTERM_DIR)/include -I$(VTERM_DIR)/src
VTERM_OBJS    = $(patsubst $(VTERM_DIR)/src/%.c,$(BUILD)/vterm_%.o,$(wildcard $(VTERM_DIR)/src/*.c))

TS_CFLAGS := -std=c11 -O3 -march=native -DNDEBUG -D_GNU_SOURCE -fno-strict-aliasing -MMD -MP -Ithird_party

MODAL_OBJS := $(BUILD)/search.o $(BUILD)/selection_set.o $(BUILD)/modal.o \
              $(BUILD)/buffers.o $(BUILD)/config.o $(BUILD)/picker.o $(BUILD)/vfx.o \
              $(BUILD)/theme.o $(BUILD)/session.o
APP_OBJS := $(BUILD)/main.o $(BUILD)/document.o $(BUILD)/syntax.o \
            $(BUILD)/syntax_queries.o $(BUILD)/font.o $(BUILD)/frame.o \
            $(BUILD)/render_vk.o $(MODAL_OBJS)
CORE_OBJS := $(BUILD)/document.o $(BUILD)/syntax.o $(BUILD)/syntax_queries.o
GRAMMAR_OBJS := $(BUILD)/c_parser.o $(BUILD)/json_parser.o \
                $(BUILD)/rust_parser.o $(BUILD)/rust_scanner.o \
                $(BUILD)/python_parser.o $(BUILD)/python_scanner.o \
                $(BUILD)/javascript_parser.o $(BUILD)/javascript_scanner.o
TS_OBJS := $(BUILD)/tree_sitter.o $(GRAMMAR_OBJS)

# Test executables live in build/ so the source tree never fills up with them;
# `make` (all: quill) still builds only the editor, and `make check` is the only
# thing that compiles tests.
TEST_NAMES := core syntax c_highlight lang_highlight highlight_cache font frame \
              modal modal_spec picker animation vfx input vk appearance theme \
              session terminal
TEST_BINS  := $(addprefix $(BUILD)/test_,$(TEST_NAMES))

.PHONY: all check clean shaders glfw submodule prune-glfw upstream-vterm

all: quill

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tree_sitter.o: third_party/tree-sitter/lib.c | $(BUILD)
	$(CC) $(TS_CFLAGS) -c $< -o $@

$(BUILD)/%_parser.o: third_party/%/parser.c | $(BUILD)
	$(CC) $(TS_CFLAGS) -Ithird_party/$* -c $< -o $@

$(BUILD)/%_scanner.o: third_party/%/scanner.c | $(BUILD)
	$(CC) $(TS_CFLAGS) -Ithird_party/$* -c $< -o $@

$(BUILD)/vterm_%.o: $(VTERM_DIR)/src/%.c | $(BUILD)
	$(CC) $(VTERM_CFLAGS) -c $< -o $@

# --- vendored GLFW (static, Wayland only) -----------------------------------
# Source is the submodule, so a fresh clone needs `git submodule update --init`.
$(GLFW_DIR)/CMakeLists.txt:
	@echo "third_party/glfw is empty -- run: git submodule update --init --recursive" >&2; exit 1

$(GLFW_PRUNE_STAMP): $(GLFW_DIR)/CMakeLists.txt
	rm -rf $(GLFW_DIR)/docs $(GLFW_DIR)/tests $(GLFW_DIR)/examples
	rm -f  $(GLFW_DIR)/deps/nuklear.h $(GLFW_DIR)/deps/nuklear_glfw_gl2.h \
	        $(GLFW_DIR)/deps/linmath.h $(GLFW_DIR)/deps/stb_image_write.h \
	        $(GLFW_DIR)/deps/tinycthread.c $(GLFW_DIR)/deps/tinycthread.h \
	        $(GLFW_DIR)/deps/getopt.c $(GLFW_DIR)/deps/getopt.h
	rm -rf $(GLFW_DIR)/deps/glad $(GLFW_DIR)/deps/mingw
	@touch $@
	@echo "pruned unused GLFW sources: third_party/glfw is now $$(du -sh $(GLFW_DIR) | cut -f1)"

prune-glfw: $(GLFW_PRUNE_STAMP)

$(GLFW_BUILD)/.configured: $(GLFW_DIR)/CMakeLists.txt $(GLFW_PRUNE_STAMP)
	@command -v cmake >/dev/null || { echo "cmake is required to build vendored GLFW" >&2; exit 1; }
	cmake -S $(GLFW_DIR) -B $(GLFW_BUILD) $(GLFW_CMAKE_FLAGS)
	@touch $@

$(GLFW_LIB): $(GLFW_BUILD)/.configured
	cmake --build $(GLFW_BUILD) --target glfw -j

glfw: $(GLFW_LIB)

# Upstream ships 3.3 MB of material this project never builds: docs, tests and
# examples, plus deps/ that only examples/tests use (nuklear, glad, mingw,
# stb_image_write, tinycthread, linmath, getopt).  deps/wayland/*.xml is NOT
# prunable -- GLFW's CMake generates headers from it via wayland-scanner and
# requires it unconditionally when GLFW_BUILD_WAYLAND is on.
# Runs once, then stamps; a fresh clone gets the same reduction automatically.
# --depth 1 keeps the submodule's full 4799-commit history out of .git/modules,
# which is otherwise the single largest thing in a checkout.  Plain
# `git submodule update` ignores submodule.*.shallow on many git versions.
submodule:
	git submodule update --init --depth 1 --single-branch --recursive
	@$(MAKE) --no-print-directory prune-glfw

quill: $(APP_OBJS) $(BUILD)/terminal.o $(TS_OBJS) $(VTERM_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $(APP_OBJS) $(BUILD)/terminal.o $(TS_OBJS) $(VTERM_OBJS) $(LDLIBS)

shaders: src/spv.h

src/spv.h: shaders/solid.vert shaders/solid.frag shaders/glyph.vert shaders/glyph.frag shaders/vfx.vert shaders/vfx.frag shaders/bg.vert shaders/bg.frag
	mkdir -p src/spv
	glslangValidator -V shaders/solid.vert -o src/spv/solid.vert.spv
	glslangValidator -V shaders/solid.frag -o src/spv/solid.frag.spv
	glslangValidator -V shaders/glyph.vert -o src/spv/glyph.vert.spv
	glslangValidator -V shaders/glyph.frag -o src/spv/glyph.frag.spv
	glslangValidator -V shaders/vfx.vert -o src/spv/vfx.vert.spv
	glslangValidator -V shaders/vfx.frag -o src/spv/vfx.frag.spv
	glslangValidator -V shaders/bg.vert -o src/spv/bg.vert.spv
	glslangValidator -V shaders/bg.frag -o src/spv/bg.frag.spv
	python3 tools/gen_spv.py

$(BUILD)/test_core.o: tests/test_core.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_syntax.o: tests/test_syntax.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_c_highlight.o: tests/test_c_highlight.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_lang_highlight.o: tests/test_lang_highlight.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_highlight_cache.o: tests/test_highlight_cache.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_font.o: tests/test_font.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_frame.o: tests/test_frame.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_vk.o: tests/test_vk.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_modal.o: tests/test_modal.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_picker.o: tests/test_picker.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_animation.o: tests/test_animation.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_vfx.o: tests/test_vfx.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_input.o: tests/test_input.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_appearance.o: tests/test_appearance.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_theme.o: tests/test_theme.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_session.o: tests/test_session.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_terminal.o: tests/test_terminal.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_modal_spec.o: tests/test_modal_spec.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_core: $(BUILD)/test_core.o $(CORE_OBJS) $(TS_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_syntax: $(BUILD)/test_syntax.o $(CORE_OBJS) $(TS_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_c_highlight: $(BUILD)/test_c_highlight.o $(CORE_OBJS) $(TS_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_lang_highlight: $(BUILD)/test_lang_highlight.o $(CORE_OBJS) $(TS_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_highlight_cache: $(BUILD)/test_highlight_cache.o $(CORE_OBJS) $(TS_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_font: $(BUILD)/test_font.o $(BUILD)/font.o $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_frame: $(BUILD)/test_frame.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(MODAL_OBJS) $(TS_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_modal: $(BUILD)/test_modal.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(MODAL_OBJS) $(TS_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_picker: $(BUILD)/test_picker.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(MODAL_OBJS) $(TS_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_animation: $(BUILD)/test_animation.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(MODAL_OBJS) $(TS_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_vfx: $(BUILD)/test_vfx.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(MODAL_OBJS) $(TS_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_input: $(BUILD)/test_input.o $(filter-out $(BUILD)/main.o,$(APP_OBJS)) $(BUILD)/terminal.o $(TS_OBJS) $(VTERM_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_vk: $(BUILD)/test_vk.o $(APP_OBJS) $(TS_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/test_vk.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(BUILD)/render_vk.o $(MODAL_OBJS) $(TS_OBJS) $(LDLIBS)
$(BUILD)/test_appearance: $(BUILD)/test_appearance.o $(APP_OBJS) $(TS_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/test_appearance.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(BUILD)/render_vk.o $(MODAL_OBJS) $(TS_OBJS) $(LDLIBS)
$(BUILD)/test_theme: $(BUILD)/test_theme.o $(BUILD)/theme.o $(BUILD)/config.o $(BUILD)/vfx.o $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_session: $(BUILD)/test_session.o $(CORE_OBJS) $(BUILD)/buffers.o $(BUILD)/session.o $(TS_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_terminal: $(BUILD)/test_terminal.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(BUILD)/terminal.o $(MODAL_OBJS) $(TS_OBJS) $(VTERM_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_modal_spec: $(BUILD)/test_modal_spec.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(MODAL_OBJS) $(TS_OBJS) $(VTERM_OBJS) $(GLFW_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# `make test_vfx` etc. still work, but produce build/test_vfx only.
test_%: $(BUILD)/test_%
	@

# libvterm's own upstream suite, pinned to the commit our vendored copy matches.
upstream-vterm: $(VTERM_OBJS)
	sh tests/run_upstream_libvterm.sh

check: quill $(TEST_BINS)
	$(BUILD)/test_core
	$(BUILD)/test_syntax
	$(BUILD)/test_c_highlight
	$(BUILD)/test_lang_highlight
	$(BUILD)/test_highlight_cache
	$(BUILD)/test_font
	$(BUILD)/test_frame
	$(BUILD)/test_modal
	$(BUILD)/test_modal_spec
	$(BUILD)/test_picker
	$(BUILD)/test_animation
	$(BUILD)/test_vk
	$(BUILD)/test_vfx
	$(BUILD)/test_input
	$(BUILD)/test_appearance
	$(BUILD)/test_theme
	$(BUILD)/test_session
	$(BUILD)/test_terminal
	./quill --headless assets/sample.c --screenshot $(BUILD)/sample.png --validation
	./quill --headless assets/sample.rs --screenshot $(BUILD)/rust.png --validation
	./quill --headless assets/sample.py --screenshot $(BUILD)/python.png --validation
	sh tests/run_upstream_libvterm.sh

TEST_OBJS := $(BUILD)/test_core.o $(BUILD)/test_syntax.o $(BUILD)/test_c_highlight.o $(BUILD)/test_lang_highlight.o $(BUILD)/test_highlight_cache.o $(BUILD)/test_font.o $(BUILD)/test_frame.o $(BUILD)/test_modal.o $(BUILD)/test_picker.o $(BUILD)/test_animation.o $(BUILD)/test_vfx.o $(BUILD)/test_input.o $(BUILD)/test_vk.o $(BUILD)/test_appearance.o $(BUILD)/test_theme.o $(BUILD)/test_session.o $(BUILD)/test_terminal.o $(BUILD)/test_modal_spec.o
DEPS := $(APP_OBJS:.o=.d) $(TS_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(BUILD)/terminal.d $(VTERM_OBJS:.o=.d)
-include $(DEPS)

clean:
	rm -rf $(BUILD) quill
	@# Older checkouts dropped test binaries in the repo root before tests moved
	@# under build/; remove them too so a clean tree really is clean.
	@rm -f test_core test_syntax test_c_highlight test_lang_highlight \
	       test_highlight_cache test_font test_frame test_modal test_modal_spec \
	       test_picker test_animation test_vk test_vfx test_input \
	       test_appearance test_theme test_session test_terminal bench_frame
