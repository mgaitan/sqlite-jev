CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -fPIC -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS ?=
LDFLAGS ?=

VERSION := $(shell tr -d '[:space:]' < VERSION)
CPPFLAGS += -DJEV_VERSION='"$(VERSION)"'
BUILD_DIR := build
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
EXT_SUFFIX := dylib
SHARED_FLAGS := -dynamiclib -undefined dynamic_lookup
DL_LIBS :=
PLATFORM := macos
else
EXT_SUFFIX := so
SHARED_FLAGS := -shared
DL_LIBS := -ldl
PLATFORM := linux
endif

EXTENSION := $(BUILD_DIR)/jev.$(EXT_SUFFIX)
PACKAGE_NAME ?= sqlite-jev-$(VERSION)-$(PLATFORM)-$(UNAME_M)
PACKAGE_DIR := $(BUILD_DIR)/$(PACKAGE_NAME)

.PHONY: all clean test integration-test live-test package

all: $(EXTENSION)

$(EXTENSION): src/jev.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SHARED_FLAGS) -o $@ $< $(LDFLAGS) $(DL_LIBS)

$(BUILD_DIR):
	mkdir -p $@

test: $(EXTENSION)
	./test/run.sh

live-test: $(EXTENSION)
	./examples/run_ticket_triage.sh

integration-test: $(EXTENSION)
	./test/run_live.sh

package: $(EXTENSION)
	rm -rf $(PACKAGE_DIR)
	mkdir -p $(PACKAGE_DIR)
	cp $(EXTENSION) $(PACKAGE_DIR)/
	cp README.md VERSION $(PACKAGE_DIR)/
	tar -czf $(BUILD_DIR)/$(PACKAGE_NAME).tar.gz -C $(BUILD_DIR) $(PACKAGE_NAME)
	@echo "Created $(BUILD_DIR)/$(PACKAGE_NAME).tar.gz"

clean:
	rm -rf $(BUILD_DIR)
