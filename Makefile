CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread

TARGET = miniredis

SRC = src/main.cpp src/server.cpp src/resp_parser.cpp src/command_handler.cpp \
	src/key_value_store.cpp src/append_only_log.cpp

$(TARGET): $(SRC) src/server.h src/resp_parser.h src/command_handler.h \
	src/key_value_store.h src/append_only_log.h
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

.PHONY: clean
clean:
	rm -f $(TARGET)