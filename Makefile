CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE
LDFLAGS = -lssl -lcrypto -lpthread

INCDIR  = include
SRCDIR  = src
TESTDIR = tests
BUILDDIR = build

CFLAGS += -I$(INCDIR)

# Test code uses runtime-length g_tmpbase in snprintf; gcc can't prove
# the 1024-byte buffers are always sufficient (they are), so suppress
# the false-positive truncation warning for test.o only.
TEST_CFLAGS = $(CFLAGS) -Wno-format-truncation

# ── Library ────────────────────────────────────────────────────────────────────

LIB_SRC = $(SRCDIR)/quine.c
LIB_OBJ = $(BUILDDIR)/quine.o

$(BUILDDIR)/quine.o: $(LIB_SRC) $(INCDIR)/quine/quine.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/libquine.a: $(LIB_OBJ)
	ar rcs $@ $^

$(BUILDDIR)/libquine.so: $(LIB_SRC) $(INCDIR)/quine/quine.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $(LIB_SRC) $(LDFLAGS)

# ── CLI (convenience) ─────────────────────────────────────────────────────────

$(BUILDDIR)/main.o: main.c $(INCDIR)/quine/quine.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/quine: $(BUILDDIR)/main.o $(BUILDDIR)/libquine.a
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ── Tests ─────────────────────────────────────────────────────────────────────

$(BUILDDIR)/test.o: $(TESTDIR)/test.c $(INCDIR)/quine/quine.h | $(BUILDDIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(BUILDDIR)/test_quine: $(BUILDDIR)/test.o $(BUILDDIR)/libquine.a
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ── Targets ───────────────────────────────────────────────────────────────────

all: $(BUILDDIR)/libquine.a $(BUILDDIR)/quine $(BUILDDIR)/test_quine

test: $(BUILDDIR)/test_quine
	$(BUILDDIR)/test_quine

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)

.PHONY: all test clean
