CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Ilib -Isrc
SRC = $(wildcard src/*.cpp)
OBJ = $(SRC:.cpp=.o)
TARGET = vliw_scheduler

all: $(TARGET)
$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^
clean:
	rm -f src/*.o $(TARGET)