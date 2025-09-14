SRC=main.c io.c mem.c c.c lex.c type.c targ.c eval.c ir.c irdump.c ssa.c intrin.c abi0.c \
	optmem.c regalloc.c amd64/sysv.c amd64/isel.c amd64/emit.c obj.c elf.c
CFLAGS=-Wall -std=c11 -pedantic
OBJ=$(patsubst %.c,obj/%.o,$(SRC))
DEP=$(OBJ:.o=.d)
OUT=antcc

all: CFLAGS += -g
all: $(OUT)

opt: CFLAGS += -O2
opt: $(OUT)

dbg: CFLAGS += -g -fsanitize=address -fsanitize=undefined
dbg: $(OUT)

$(OUT): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

obj/%.o: %.c common.h
	@mkdir -p `dirname $@`
	$(CC) $(CFLAGS) -MMD -MP -MT $@ -MF obj/$*.d -c -o $@ $<

clean:
	$(RM) -r obj/ $(OUT) *.o a.out

.PHONY: clean

-include $(DEP)
