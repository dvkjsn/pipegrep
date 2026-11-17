CXX = g++
CXXFLAGS = -std=c++17 -pthread

# builds the pipegrep executable from main.cpp
pipegrep: main.cpp BoundedBuffer.h
	$(CXX) $(CXXFLAGS) -o pipegrep main.cpp

# removes the executable only; source files are untouched
clean:
	rm -f pipegrep

.PHONY: clean