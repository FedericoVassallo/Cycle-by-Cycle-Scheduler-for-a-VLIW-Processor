CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Iinclude -Ilib -Isrc
SRC = $(wildcard src/*.cpp)
BUILD_DIR = build
OBJ = $(SRC:src/%.cpp=$(BUILD_DIR)/%.o)
TARGET = vliw_scheduler

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -rf $(BUILD_DIR) $(TARGET)