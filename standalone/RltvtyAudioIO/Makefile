# Build this plugin against the Rack SDK.
# Export RACK_DIR in your shell, e.g.
# export RACK_DIR=/Users/epinzur/src/github.com/rltvty/Rack-SDK
ifndef RACK_DIR
$(error RACK_DIR is not defined)
endif

FLAGS +=
CFLAGS +=
CXXFLAGS +=
LDFLAGS +=

SOURCES += $(wildcard src/*.cpp)

DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard presets)

include $(RACK_DIR)/plugin.mk
