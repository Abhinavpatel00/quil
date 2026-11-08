# Quill -- Vulkan editor on mu_quill. Tree-sitter only; no LSP/completion.
MYLIB   ?= ../mylib
CC      ?= cc
PKGCONFIG ?= pkg-config
# prefer glfw static linking
GLFW_LIB := /home/lk/myprojects/voxelfun/mu_editor/build/glfw/src/libglfw3.a
CFLAGS  := -std=c99 -O2 -Wall -Wextra -Werror -fno-strict-aliasing -march=native -D_GNU_SOURCE -MMD -MP
CFLAGS  += -I$(MYLIB) -Isrc -Ithird_party -Ithird_party/tree-sitter -Ithird_party/libvterm/include
CFLAGS  += $(shell $(PKGCONFIG) --cflags freetype2 harfbuzz fontconfig  vulkan)
LDLIBS := $(shell $(PKGCONFIG) --libs freetype2 harfbuzz fontconfig vulkan) -lm
LDLIBS += $(GLFW_LIB) -lpthread
# Vendored libvterm (MIT): VT escape-sequence parser + screen model for the
# integrated terminal. Built without -Werror (third-party, -Wall -Wpedantic clean).
VTERM_DIR    := third_party/libvterm
VTERM_CFLAGS := -std=c99 -O2 -march=native -fno-strict-aliasing -MMD -MP \
                -I$(VTERM_DIR)/include -I$(VTERM_DIR)/src
VTERM_OBJS    = $(patsubst $(VTERM_DIR)/src/%.c,$(BUILD)/vterm_%.o,$(wildcard $(VTERM_DIR)/src/*.c))

TS_CFLAGS := -std=c11 -O3 -march=native -DNDEBUG -D_GNU_SOURCE -fno-strict-aliasing -MMD -MP -Ithird_party

BUILD := build
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

.PHONY: all check clean shaders

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

quill: $(APP_OBJS) $(BUILD)/terminal.o $(TS_OBJS) $(VTERM_OBJS)
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

test_core: $(BUILD)/test_core.o $(CORE_OBJS) $(TS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
test_syntax: $(BUILD)/test_syntax.o $(CORE_OBJS) $(TS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
test_c_highlight: $(BUILD)/test_c_highlight.o $(CORE_OBJS) $(TS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
test_lang_highlight: $(BUILD)/test_lang_highlight.o $(CORE_OBJS) $(TS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
test_highlight_cache: $(BUILD)/test_highlight_cache.o $(CORE_OBJS) $(TS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
test_font: $(BUILD)/test_font.o $(BUILD)/font.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
test_frame: $(BUILD)/test_frame.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(MODAL_OBJS) $(TS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
test_modal: $(BUILD)/test_modal.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(MODAL_OBJS) $(TS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
test_picker: $(BUILD)/test_picker.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(MODAL_OBJS) $(TS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
test_animation: $(BUILD)/test_animation.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(MODAL_OBJS) $(TS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
test_vfx: $(BUILD)/test_vfx.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(MODAL_OBJS) $(TS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
test_input: $(BUILD)/test_input.o $(filter-out $(BUILD)/main.o,$(APP_OBJS)) $(BUILD)/terminal.o $(TS_OBJS) $(VTERM_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
test_vk: $(BUILD)/test_vk.o $(APP_OBJS) $(TS_OBJS)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/test_vk.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(BUILD)/render_vk.o $(MODAL_OBJS) $(TS_OBJS) $(LDLIBS)
test_appearance: $(BUILD)/test_appearance.o $(APP_OBJS) $(TS_OBJS)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/test_appearance.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(BUILD)/render_vk.o $(MODAL_OBJS) $(TS_OBJS) $(LDLIBS)
test_theme: $(BUILD)/test_theme.o $(BUILD)/theme.o $(BUILD)/config.o $(BUILD)/vfx.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
test_session: $(BUILD)/test_session.o $(CORE_OBJS) $(BUILD)/buffers.o $(BUILD)/session.o $(TS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
test_terminal: $(BUILD)/test_terminal.o $(CORE_OBJS) $(BUILD)/font.o $(BUILD)/frame.o $(BUILD)/terminal.o $(MODAL_OBJS) $(TS_OBJS) $(VTERM_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

check: quill test_core test_syntax test_c_highlight test_lang_highlight test_highlight_cache test_font test_frame test_modal test_picker test_animation test_vk test_vfx test_input test_appearance test_theme test_session test_terminal
	./test_core
	./test_syntax
	./test_c_highlight
	./test_lang_highlight
	./test_highlight_cache
	./test_font
	./test_frame
	./test_modal
	./test_picker
	./test_animation
	./test_vk
	./test_vfx
	./test_input
	./test_appearance
	./test_theme
	./test_session
	./test_terminal
	./quill --headless assets/sample.c --screenshot $(BUILD)/sample.png --validation
	./quill --headless assets/sample.rs --screenshot $(BUILD)/rust.png --validation
	./quill --headless assets/sample.py --screenshot $(BUILD)/python.png --validation

TEST_OBJS := $(BUILD)/test_core.o $(BUILD)/test_syntax.o $(BUILD)/test_c_highlight.o $(BUILD)/test_lang_highlight.o $(BUILD)/test_highlight_cache.o $(BUILD)/test_font.o $(BUILD)/test_frame.o $(BUILD)/test_modal.o $(BUILD)/test_picker.o $(BUILD)/test_animation.o $(BUILD)/test_vfx.o $(BUILD)/test_input.o $(BUILD)/test_vk.o $(BUILD)/test_appearance.o $(BUILD)/test_theme.o $(BUILD)/test_session.o $(BUILD)/test_terminal.o
DEPS := $(APP_OBJS:.o=.d) $(TS_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(BUILD)/terminal.d $(VTERM_OBJS:.o=.d)
-include $(DEPS)

clean:
	rm -rf $(BUILD) quill test_core test_syntax test_c_highlight test_lang_highlight test_highlight_cache test_font test_frame test_modal test_picker test_animation test_vk test_vfx test_input test_appearance test_theme test_session test_terminal
