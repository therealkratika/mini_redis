CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread

TARGET = miniredis
CLI_TARGET = miniredis-cli

CORE_SRC = src/server.cpp src/resp_parser.cpp src/command_handler.cpp \
	src/key_value_store.cpp src/append_only_log.cpp src/client_connection.cpp \
	src/pub_sub.cpp src/replication.cpp

UNIT_SRC = src/resp_parser.cpp src/command_handler.cpp src/key_value_store.cpp \
	src/append_only_log.cpp src/client_connection.cpp src/pub_sub.cpp \
	src/replication.cpp

all: $(TARGET) $(CLI_TARGET)

$(TARGET): src/main.cpp $(CORE_SRC) src/server.h src/resp_parser.h src/command_handler.h \
	src/key_value_store.h src/append_only_log.h src/client_connection.h \
	src/pub_sub.h src/replication.h
	$(CXX) $(CXXFLAGS) src/main.cpp $(CORE_SRC) -o $(TARGET)

$(CLI_TARGET): src/cli_main.cpp
	$(CXX) $(CXXFLAGS) src/cli_main.cpp -o $(CLI_TARGET)

unit_tests: tests/unit_tests.cpp $(UNIT_SRC)
	$(CXX) $(CXXFLAGS) -Isrc tests/unit_tests.cpp $(UNIT_SRC) -o unit_tests

network_tests: tests/network_tests.cpp $(CORE_SRC)
	$(CXX) $(CXXFLAGS) -Isrc tests/network_tests.cpp $(CORE_SRC) -o network_tests

test: unit_tests
	./unit_tests

test-network: network_tests
	./network_tests

.PHONY: all clean test test-network
clean:
	rm -f $(TARGET) $(CLI_TARGET) unit_tests network_tests