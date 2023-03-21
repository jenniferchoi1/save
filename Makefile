CXX = g++
CXXFLAGS = -g -std=c++11 -pedantic

all: miProxy 
miProxy: miProxy.cpp
	$(CXX) $(CXXFLAGS) miProxy.cpp -o miProxy

clean:
	rm -rf miProxy *.dSYM

.PHONY: clean
