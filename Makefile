SRC=main.c io.c mem.c parse.c lex.c type.c targ.c eval.c ir.c irdump.c abistruct.c regalloc.c amd64/sysv.c
CFLAGS=-Wall -std=c11 -pedantic
OBJ=$(patsubst %.c,obj/%.o,$(SRC))
OUT=cchomp

all: CFLAGS += -g -Og
all: $(OUT)

opt: CFLAGS += -O2
opt: $(OUT)

dbg: CFLAGS += -g -fsanitize=address -fsanitize=undefined
dbg: $(OUT)

$(OUT): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

obj/%.o: %.c common.h
	@mkdir -p `dirname $@`
	$(CC) $(CFLAGS) -c -o $@ $<

ir.h: op.def builtin.def
obj/main.o: parse.h
obj/parse.o: parse.h ir.h
obj/ir.o: ir.h
obj/irdump.o: ir.h
obj/lex.o: parse.h
obj/eval.o: parse.h
obj/io.o: parse.h keywords.def
obj/abistruct.o: ir.h
obj/regalloc.o: ir.h
obj/amd64/sysv.o: ir.h amd64/all.h

clean:
	$(RM) -r obj/ $(OUT)

.PHONY: clean
