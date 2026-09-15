CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra

TARGET = miniredis

SRC = src/main.cpp src/server.cpp src/resp_parser.cpp src/command_handler.cpp

$(TARGET): $(SRC) src/server.h src/resp_parser.h src/command_handler.h
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

.PHONY: clean
clean:
	rm -f $(TARGET)