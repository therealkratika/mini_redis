CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra

TARGET = miniredis

SRC = src/main.cpp src/server.cpp

$(TARGET):
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)