OUT?=antcc
BUILDDIR?=build
srcs=a_main.c           a_targ.c          a_cincludes.c      a_rtpack.c   a_sys.c       gen_libantrt1.c \
     c_builtin.c        c.c               c_eval.c           c_lex.c      c_type.c        \
     ir_abi0.c          ir_builder.c      ir.c               ir_cfg.c     ir_cse.c        \
     ir_dump.c          ir_fold.c         ir_inliner.c       ir_intrin.c  ir_mem2reg.c    \
     ir_regalloc.c      ir_simpl.c        ir_ssa.c           ir_stack.c   ir_check.c      \
     ir_loop.c          ir_motion.c                                                       \
     obj.c              o_elf.c                                                           \
     t_aarch64_aapcs.c  t_aarch64_emit.c  t_aarch64_isel.c                                \
     t_x86-64_emit.c    t_x86-64_isel.c   t_x86-64_sysv.c                                 \
     u_io.c             u_mem.c                                                           \
     z_miniz.c
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

# hack to make sure version.h is kept up to date: always gen version, but to a temp file
# that is moved to the real version.h only if it differs, to avoid rebuilds every `make`
# hmm, I don't love this..
src/version.h: VERSION FORCE
	@t=$@.tmp.$$$$; \
	trap 'rm -f "$$t"' EXIT; \
	tool/gen-version.sh > "$$t" && \
	{ cmp -s "$$t" $@ || mv -f "$$t" $@; }

FORCE:

src/a_cincludes.c: src/a_cincludes.sh
	src/a_cincludes.sh > $@
	@mkdir -p $(BUILDDIR)
	$(TOOLCC) -DCINCLUDES_CHECKSIZES $@ -o $(BUILDDIR)/a_cincludes_checksizes
	$(BUILDDIR)/a_cincludes_checksizes

tool/mkpack: tool/mkpack.c src/z_miniz.c src/z_miniz.h
	$(TOOLCC) -Wall -g -o $@ $<

src/gen_libantrt1.c: tool/mkpack $(wildcard src/libantrt1/*)
	tool/mkpack src/libantrt1 src/gen_libantrt1.c

$(BUILDDIR)/z_miniz.o: CFLAGS += -Wno-unused-function

$(BUILDDIR)/%.o: src/%.c | src/version.h
	$Vmkdir -p `dirname $@`
	$(CC) $(CFLAGS) -c -o $@ $<
	$Vtool/depgen -MP -MF $(BUILDDIR)/$*.d -MT $@ $<

clean:
	$(RM) -r -- $(BUILDDIR)/ test/build/ $(OUT) *.o a.out

clean-tool:
	$(RM) tool/depgen tool/mkpack

clean-config: clean
	$(RM) config.mk src/hostconfig.h

install: all
	@mkdir -p "$(DESTDIR)$(BINDIR)"
	install -m755 $(OUT) -T "$(DESTDIR)$(BINDIR)/antcc"

uninstall:
	$(RM) "$(DESTDIR)$(BINDIR)/antcc"

.PHONY: clean clean-config install uninstall FORCE

-include $(dep)

.DELETE_ON_ERROR:
