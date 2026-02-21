OUT?=antcc
BUILDDIR?=build
SRC=main.c io.c mem.c c/c.c c/lex.c c/eval.c c/builtin.c type.c targ.c \
	ir/ir.c ir/builder.c ir/fold.c ir/dump.c ir/ssa.c ir/cfg.c ir/intrin.c ir/abi0.c ir/mem2reg.c ir/regalloc.c ir/simpl.c ir/stack.c \
	ir/cse.c ir/inliner.c \
	x86_64/sysv.c x86_64/isel.c x86_64/emit.c \
	aarch64/aapcs.c aarch64/isel.c aarch64/emit.c \
	obj/obj.c obj/elf.c \
	embedfilesdir.c
OBJ=$(patsubst %.c,build/%.o,$(SRC))
DEP=$(OBJ:.o=.d)

CFLAGS=-Wall -std=c11 -pedantic
PREFIX=/usr/local
BINDIR=$(PREFIX)/bin

-include config.mk

all: CFLAGS += -g -Og
all: $(OUT)

opt: CFLAGS += -g -O2
opt: $(OUT)

dbg: CFLAGS += -g -fsanitize=address,undefined
dbg: CC:=clang
dbg: $(OUT)

$(OUT): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

hostconfig.h:
	./configure

$(BUILDDIR)/%.o: %.c hostconfig.h
	@mkdir -p `dirname $@`
	@cc $(CFLAGS) -MMD -MP -fsyntax-only -MF $(BUILDDIR)/$*.d -MT $@ $<  #depfiles
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) -r $(BUILDDIR)/ test/build/ $(OUT) *.o a.out

clean-config: clean
	$(RM) -r config.mk hostconfig.h

install: all
	@mkdir -p "$(DESTDIR)$(BINDIR)"
	install -m755 $(OUT) -T "$(DESTDIR)$(BINDIR)/antcc"

.PHONY: clean clean-config install

-include $(DEP)
