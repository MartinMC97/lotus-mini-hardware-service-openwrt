CXX ?= g++
CPPFLAGS += -isystem third_party/picojson
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -Werror
LDLIBS += -pthread

all: build/lotus-hardwared build/lotus-hwctl
build/control-smoke: tools/control-smoke.cpp src/connection.h src/protocol.h | build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tools/control-smoke.cpp $(LDFLAGS) $(LDLIBS) -o $@
build/quiet-tone: tools/quiet-tone.cpp | build
	$(CXX) $(CXXFLAGS) $< -o $@
build:
	mkdir -p $@
build/lotus-hardwared: src/server.cpp src/protocol.h src/async_log.h src/sensors.h src/controls.h src/led_backend.h src/input_backend.h src/volume.h src/volume_store.h src/privacy.h src/privacy_worker.h src/privacy_store.h third_party/picojson/picojson.h | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/server.cpp $(LDFLAGS) $(LDLIBS) -o $@
build/lotus-hardwared-alsa: src/server.cpp src/protocol.h src/async_log.h src/sensors.h src/controls.h src/led_backend.h src/input_backend.h src/volume.h src/volume_store.h src/privacy.h src/privacy_worker.h src/privacy_store.h third_party/picojson/picojson.h | build
	$(CXX) $(CPPFLAGS) -DHW_WITH_ALSA $(CXXFLAGS) src/server.cpp $(LDFLAGS) $(LDLIBS) -lasound -o $@
build/lotus-hwctl: src/client.cpp src/protocol.h src/connection.h third_party/picojson/picojson.h | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/client.cpp $(LDFLAGS) $(LDLIBS) -o $@
build/connection-test: tests/connection_test.cpp src/connection.h src/protocol.h | build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/connection_test.cpp $(LDFLAGS) $(LDLIBS) -o $@
build/controls-test: tests/controls_test.cpp src/controls.h src/led_backend.h src/input_backend.h src/volume.h | build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/controls_test.cpp $(LDFLAGS) $(LDLIBS) -o $@
build/store-fault-test: tests/store_fault_test.cpp src/volume_store.h src/protocol.h | build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $< $(LDFLAGS) $(LDLIBS) -Wl,--wrap=fsync -o $@
build/privacy-test: tests/privacy_test.cpp src/privacy.h src/privacy_store.h src/protocol.h | build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $< $(LDFLAGS) $(LDLIBS) -Wl,--wrap=fsync -o $@
build/application-client-test: tests/application_client_test.cpp src/application_client.h src/connection.h src/protocol.h | build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@
test: all build/connection-test build/controls-test build/quiet-tone build/store-fault-test build/privacy-test build/application-client-test
	./build/controls-test
	./build/store-fault-test
	./build/privacy-test
	python3 tests/integration.py
	python3 tests/log_policy.py
.PHONY: all test
