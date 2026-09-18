CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter -pipe
BIN      := ../bin

TARGETS := $(BIN)/mwis_engine $(BIN)/mwis_exact $(BIN)/mwis_legacy

all: $(TARGETS)

$(BIN):
	mkdir -p $(BIN)

$(BIN)/mwis_engine: mwis_engine.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BIN)/mwis_exact: mwis_exact.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ $<

# The original hackathon submission, built as-is so the correctness harness can
# compare against it. Its 290 s internal limit is left untouched.
$(BIN)/mwis_legacy: ../solution.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean
