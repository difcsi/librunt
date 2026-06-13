# replicate the multiarch build we do in libsystrap/contrib/Makefile

.PHONY: default
# We want "targets" to be the actual files that we want to build.
# But we build them by recursive 'make'. From this makefile, we have
# no way to make these target depend on their source files! So once they
# exist, they will appear not to need rebuilding. Instead we name them
# without the preceding 'build/', and in the rules where we need the
# legit path, we add 'build/' back in. This is similar to making them all
# phony targets (which I tried, using eval, but could not get to work).
TARGETS := $(foreach d,opt-i386 debug-i386 opt-x86_64 debug-x86_64,$(d)/librunt_preload.a)
$(info TARGETS is $(TARGETS))
default: $(TARGETS)

debug-i386/librunt_preload.a opt-i386/librunt_preload.a: MAKE_PREFIX := \
  CC="$(CC) -m32" CPPFLAGS="-D_FILE_OFFSET_BITS=64" ASFLAGS="-m32" LDFLAGS="-Wl,-melf_i386"

debug-%/librunt_preload.a:
	mkdir -p $(dir build/$@) && cd $(dir build/$@) && DEBUG=1 $(MAKE_PREFIX) $(MAKE) -f ../../src/Makefile
opt-%/librunt_preload.a:
	mkdir -p $(dir build/$@) && cd $(dir build/$@) && $(MAKE_PREFIX) $(MAKE) -f ../../src/Makefile

# Native (host-arch) build, into build/opt (no arch suffix). This is what
# lib/outdir and the test harness expect, so it is kept separate from the
# multiarch TARGETS above. The recursive make handles incremental rebuilds
# via its own .d files, so this is always phony.
.PHONY: native
native:
	mkdir -p build/opt && cd build/opt && $(MAKE) -f ../../src/Makefile

# Convenience symlinks under lib/ (lib/outdir -> build/opt, lib/librunt_preload.so, ...)
.PHONY: lib
lib: native
	$(MAKE) -C lib

# Build the native library and run the test suite. This is the one-stop
# target for running the tests from a clean checkout: 'make check'.
.PHONY: check test
check test: lib
	$(MAKE) -C test checkall

.PHONY: clean
clean:
	rm -rf build
	$(MAKE) -C lib clean
	$(MAKE) -C test clean
