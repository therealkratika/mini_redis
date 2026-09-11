CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra

TARGET = miniredis

SRC = src/main.cpp src/server.cpp

$(TARGET): $(SRC) src/server.h
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

.PHONY: clean
clean:
	rm -f $(TARGET)