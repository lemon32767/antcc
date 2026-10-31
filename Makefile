OUT ?= antcc
BUILDDIR ?= build
srcs=a_main.c           a_targ.c          a_cincludes.c      a_rtpack.c   a_sys.c       gen_libantrt1.c \
     c_builtin.c        c.c               c_eval.c           c_lex.c      c_type.c      c_complex.c     \
     ir_abi0.c          ir_builder.c      ir.c               ir_cfg.c     ir_cse.c                      \
     ir_dump.c          ir_fold.c         ir_inliner.c       ir_intrin.c  ir_mem2reg.c                  \
     ir_regalloc.c      ir_simpl.c        ir_ssa.c           ir_stack.c   ir_check.c                    \
     ir_loop.c          ir_motion.c                                                                     \
     obj.c              o_elf.c                                                                         \
     t_aarch64_aapcs.c  t_aarch64_emit.c  t_aarch64_isel.c                                              \
     t_x86-64_emit.c    t_x86-64_isel.c   t_x86-64_sysv.c                                               \
     u_io.c             u_mem.c                                                                         \
     z_miniz.c
libantrt1_srcs=divc3.inc      divdc3.c        divsc3.c       dso_handle.c  \
               _int.h         _math.h         mulc3.inc      muldc3.c      \
               mulsc3.c       popcountdi2.c   popcountsi2.c

src=$(srcs:%=src/%)
obj=$(src:src/%.c=$(BUILDDIR)/%.o)
dep=$(obj:.o=.d)
libantrt1_src=$(libantrt1_srcs:%=src/libantrt1/%)

-include config.mk
BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man

TOOLCC ?= cc
RM ?= rm -f

V ?= @
Q = $(V:1=)

MAKEFLAGS += --no-print-directory

all: gen-version
	@$(MAKE) CC="$(CC)" CFLAGS="$(CFLAGS) -g -Og" V="$(V)" \
	    OUT="$(OUT)" BUILDDIR="$(BUILDDIR)" $(OUT)

opt: gen-version
	@$(MAKE) CC="$(CC)" CFLAGS="$(CFLAGS) -g -O2" V="$(V)" \
	    OUT="$(OUT)" BUILDDIR="$(BUILDDIR)" $(OUT)

dbg: gen-version
	@$(MAKE) CC="clang" CFLAGS="$(CFLAGS) -g -fsanitize=address,undefined" V="$(V)" \
	    OUT="$(OUT)" BUILDDIR="$(BUILDDIR)" $(OUT)

tool/depgen: tool/depgen.c
	$(TOOLCC) -Wall -g -o $@ tool/depgen.c

$(OUT): tool/depgen $(obj)
	$(CC) -o $@ 	$(CFLAGS) $(obj)

# hack to make sure version.h is kept up to date: always gen version, but to a temp file
# that is moved to the real version.h only if it differs, to avoid rebuilds every `make`
# hmm, I don't love this..
gen-version:
	@t=$@.tmp.$$$$; \
	trap 'rm -f "$$t"' EXIT; \
	tool/gen-version.sh > "$$t" && \
	{ cmp -s "$$t" src/version.h || mv -f "$$t" src/version.h; }

src/version.h: VERSION tool/gen-version.sh
	tool/gen-version.sh > $@

src/a_cincludes.c: src/a_cincludes.sh
	src/a_cincludes.sh > $@
	@mkdir -p $(BUILDDIR)
	$(TOOLCC) -DCINCLUDES_CHECKSIZES $@ -o $(BUILDDIR)/a_cincludes_checksizes
	$(BUILDDIR)/a_cincludes_checksizes

tool/mkpack: tool/mkpack.c src/z_miniz.c src/z_miniz.h
	$(TOOLCC) -Wall -g -o $@ tool/mkpack.c

src/gen_libantrt1.c: tool/mkpack $(libantrt1_src) src/libantrt1
	tool/mkpack src/libantrt1 $@

$(BUILDDIR)/a_main.o $(BUILDDIR)/a_rtpack.o $(BUILDDIR)/c_lex.o: src/version.h
$(BUILDDIR)/a_cincludes.o: src/a_cincludes.c
$(BUILDDIR)/gen_libantrt1.o: src/gen_libantrt1.c

CFLAGS_$(BUILDDIR)/z_miniz.o = -Wno-unused-function

$(obj): tool/depgen
	$(Q)mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) $(CFLAGS_$@) -c -o $@ $(@:$(BUILDDIR)/%.o=src/%.c)
	$(Q)tool/depgen -MP -MF $(@:.o=.d) -MT $@ $(@:$(BUILDDIR)/%.o=src/%.c)

clean:
	$(RM) -r -- $(BUILDDIR)/ test/build/ $(OUT) *.o a.out

clean-tool:
	$(RM) tool/depgen tool/mkpack

clean-config: clean
	$(RM) config.mk src/hostconfig.h

install: $(OUT)
	@mkdir -p "$(DESTDIR)$(BINDIR)"
	install -m755 $(OUT) "$(DESTDIR)$(BINDIR)/antcc"
	mkdir -p "$(DESTDIR)$(MANDIR)/man1"
	"$(DESTDIR)$(BINDIR)/antcc" --man > "$(DESTDIR)$(MANDIR)/man1/antcc.1"

uninstall:
	$(RM) "$(DESTDIR)$(BINDIR)/antcc"
	$(RM) "$(DESTDIR)$(MANDIR)/man1/antcc.1"

.PHONY: all opt dbg gen-version clean clean-tool clean-config install uninstall

.DELETE_ON_ERROR:

-include $(dep)
