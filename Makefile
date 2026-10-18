CXX = g++

CXXFLAGS = -O2 -masm=intel -Wall
LDFLAGS = -shared -static -static-libgcc -static-libstdc++ -s

RenderDistanceMod.dll: main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)
