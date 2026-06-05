OUT?=antcc
BUILDDIR?=build
srcs=a_main.c           a_targ.c          a_embedfilesdir.c                            \
     c_builtin.c        c.c               c_eval.c           c_lex.c      c_type.c     \
     ir_abi0.c          ir_builder.c      ir.c               ir_cfg.c     ir_cse.c     \
     ir_dump.c          ir_fold.c         ir_inliner.c       ir_intrin.c  ir_mem2reg.c \
     ir_regalloc.c      ir_simpl.c        ir_ssa.c           ir_stack.c                \
     obj.c              o_elf.c                                                        \
     t_aarch64_aapcs.c  t_aarch64_emit.c  t_aarch64_isel.c                             \
     t_x86-64_emit.c    t_x86-64_isel.c   t_x86-64_sysv.c                              \
     u_io.c             u_mem.c                                                   
src=$(addprefix src/, $(srcs))
obj=$(patsubst src/%.c,$(BUILDDIR)/%.o,$(src))
dep=$(obj:.o=.d)

-include config.mk
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

$(OUT): tool/depgen src/version.h $(obj)
	$(CC) $(CFLAGS) -o $@ $(obj)

src/version.h: VERSION
	tool/gen-version.sh > src/version.h

src/a_embedfilesdir.c: src/a_embedfilesdir.sh
	src/a_embedfilesdir.sh > $@
	@mkdir -p $(BUILDDIR)
	$(TOOLCC) -DEMBEDFILESDIR_CHECKSIZES $@ -o $(BUILDDIR)/a_embedfilesdir_checksizes
	$(BUILDDIR)/a_embedfilesdir_checksizes

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

-include $(dep)
