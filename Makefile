CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra
LDFLAGS = -lSDL2 -lSDL2_image -lSDL2_ttf
TARGET = taskmanager
SRC = taskmanager.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
