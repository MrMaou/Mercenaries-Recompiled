# Desktop tests for the in-game Archipelago client (Linux/macOS, gcc or clang).
#   make -C ports/mercenaries/plugins/archipelago/tests check
# test_live additionally needs the APClientPP dependencies (DEPS=dir with
# apclientpp, wswrap, websocketpp, asio, json) and a running Archipelago server;
# see README.md.
ROOT := ../../../../..
LUA := $(ROOT)/third_party/lua-5.0.3
BUILD ?= build
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Wpedantic
LUA_SRC := $(filter-out %/ltests.c,$(wildcard $(LUA)/src/*.c)) \
           $(LUA)/src/lib/lauxlib.c $(LUA)/src/lib/lbaselib.c $(LUA)/src/lib/lstrlib.c \
           $(LUA)/src/lib/ltablib.c $(LUA)/src/lib/lmathlib.c
LUA_OBJ := $(patsubst $(LUA)/%.c,$(BUILD)/lua/%.o,$(LUA_SRC))
INCLUDES := -I$(LUA)/include -I$(LUA)/src

all: $(BUILD)/test_logic

$(BUILD)/lua/%.o: $(LUA)/%.c
	@mkdir -p $(dir $@)
	$(CC) -O1 -w $(INCLUDES) -c $< -o $@

$(BUILD)/test_logic: test_logic.cpp lua_game_io.hpp ../src/game_logic.cpp ../src/game_logic.hpp ../src/ap_data.inc $(LUA_OBJ)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) test_logic.cpp ../src/game_logic.cpp $(LUA_OBJ) -lm -o $@

DEPS ?= deps
$(BUILD)/test_live: test_live.cpp lua_game_io.hpp ../src/ap_session.cpp ../src/ap_session.hpp ../src/game_logic.cpp $(LUA_OBJ)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -Wno-deprecated-declarations $(INCLUDES) -DASIO_STANDALONE -DWSWRAP_NO_SSL -DAP_NO_SCHEMA \
	  -I$(DEPS)/apclientpp -I$(DEPS)/wswrap/include -I$(DEPS)/websocketpp -I$(DEPS)/asio/asio/include \
	  -I$(DEPS)/json/include test_live.cpp ../src/ap_session.cpp ../src/game_logic.cpp $(LUA_OBJ) -lm -lz -lpthread -o $@

check: $(BUILD)/test_logic
	$(BUILD)/test_logic game_stubs.lua

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
