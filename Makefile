OUT?=antcc
BUILDDIR?=build
SRC=$(wildcard src/*.c)
OBJ=$(patsubst src/%.c,build/%.o,$(SRC))
DEP=$(OBJ:.o=.d)

CFLAGS=-Wall -std=c11 -pedantic
PREFIX=/usr/local
BINDIR=$(PREFIX)/bin

-include config.mk

TOOLCC ?= cc

ifdef V
	V=
else
	V=@
endif

all: CFLAGS += -g -Og
all: $(OUT)

opt: CFLAGS += -g -O2
opt: $(OUT)

dbg: CFLAGS += -g -fsanitize=address,undefined
dbg: CC:=clang
dbg: $(OUT)

tool/depgen: tool/depgen.c
	$(TOOLCC) -Wall -g -o $@ $<

$(OUT): tool/depgen $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

src/hostconfig.h:
	./configure

$(BUILDDIR)/%.o: src/%.c src/hostconfig.h
	$Vmkdir -p `dirname $@`
	$Vtool/depgen -MP -MF $(BUILDDIR)/$*.d -MT $@ $<
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) -r -- $(BUILDDIR)/ test/build/ $(OUT) *.o a.out

clean-tool:
	$(RM) -r tool/depgen

clean-config: clean
	$(RM) -r config.mk src/hostconfig.h

install: all
	@mkdir -p "$(DESTDIR)$(BINDIR)"
	install -m755 $(OUT) -T "$(DESTDIR)$(BINDIR)/antcc"

.PHONY: clean clean-config install

-include $(DEP)
