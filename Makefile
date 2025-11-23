SRC=main.c io.c mem.c c/c.c c/lex.c c/eval.c c/builtin.c type.c targ.c \
	ir/ir.c ir/builder.c ir/fold.c ir/dump.c ir/ssa.c ir/cfg.c ir/intrin.c ir/abi0.c ir/optmem.c ir/regalloc.c \
	amd64/sysv.c amd64/isel.c amd64/emit.c obj/obj.c obj/elf.c \
	embedfilesdir.c
CFLAGS=-Wall -std=c11 -pedantic
OBJ=$(patsubst %.c,build/%.o,$(SRC))
DEP=$(OBJ:.o=.d)
BUILDDIR?=build
OUT?=antcc

all: CFLAGS += -g
all: $(OUT)

opt: CFLAGS += -g -O2
opt: $(OUT)

dbg: CFLAGS += -g -fsanitize=address,undefined
dbg: CC:=clang
dbg: $(OUT)

$(OUT): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

$(BUILDDIR)/%.o: %.c common.h
	@mkdir -p `dirname $@`
	$(CC) $(CFLAGS) -MMD -MP -MT $@ -MF $(BUILDDIR)/$*.d -c -o $@ $<

clean:
	$(RM) -r $(BUILDDIR)/ test/build/ $(OUT) *.o a.out

.PHONY: clean

-include $(DEP)
