OUT?=antcc
BUILDDIR?=build
SRC=$(wildcard src/*.c)
OBJ=$(patsubst src/%.c,$(BUILDDIR)/%.o,$(SRC))
DEP=$(OBJ:.o=.d)

include config.mk
BINDIR=$(PREFIX)/bin


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

src/a_embedfilesdir.c: src/a_embedfilesdir.sh
	src/a_embedfilesdir.sh > src/a_embedfilesdir.c

$(BUILDDIR)/%.o: src/%.c
	$Vmkdir -p `dirname $@`
	$(CC) $(CFLAGS) -c -o $@ $<
	$Vtool/depgen -MP -MF $(BUILDDIR)/$*.d -MT $@ $<

clean:
	$(RM) -r -- $(BUILDDIR)/ test/build/ $(OUT) *.o a.out

clean-tool:
	$(RM) tool/depgen

clean-config: clean
	$(RM) config.mk src/hostconfig.h

install: all
	@mkdir -p "$(DESTDIR)$(BINDIR)"
	install -m755 $(OUT) -T "$(DESTDIR)$(BINDIR)/antcc"

uninstall:
	$(RM) "$(DESTDIR)$(BINDIR)/antcc"

.PHONY: clean clean-config install uninstall

-include $(DEP)
