CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra -I.. -I../engines/scumm -I../common -I../test
LDFLAGS = -lgtest -lgtest_main -lpthread

TEST_SRCS = mcp_test.cpp
TEST_OBJS = $(TEST_SRCS:.cpp=.o)
TEST_BIN = mcp_test

all: $(TEST_BIN)

$(TEST_BIN): $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(TEST_OBJS) $(TEST_BIN)

run: $(TEST_BIN)
	./$(TEST_BIN)

.PHONY: all clean run