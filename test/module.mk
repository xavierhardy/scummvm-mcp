######################################################################
# Unit/regression tests, based on CxxTest.
# Use the 'test' target to run them.
# Edit TESTS and TESTLIBS to add more tests.
#
######################################################################

TESTS        := $(srcdir)/test/common/*.h \
	$(srcdir)/test/common/compression/*.h \
	$(srcdir)/test/common/formats/*.h \
	$(srcdir)/test/audio/*.h \
	$(srcdir)/test/math/*.h \
	$(srcdir)/test/image/*.h
TEST_LIBS    :=

ifdef POSIX
TEST_LIBS += test/system/null_osystem.o \
	backends/fs/posix/posix-fs-factory.o \
	backends/fs/posix/posix-fs.o \
	backends/fs/posix/posix-iostream.o \
	backends/fs/abstract-fs.o \
	backends/fs/stdiostream.o \
	backends/modular-backend.o
endif

ifdef WIN32
TEST_LIBS += test/system/null_osystem.o \
	backends/fs/windows/windows-fs-factory.o \
	backends/fs/windows/windows-fs.o \
	backends/fs/abstract-fs.o \
	backends/fs/stdiostream.o \
	backends/modular-backend.o \
	backends/platform/sdl/win32/win32_wrapper.o
endif

ifdef USE_TINYGL
TESTS += $(srcdir)/test/graphics/tinygl*.h
endif

# libcommon needs libformats and libformats needs libcommon: so libcommon is put twice
TEST_LIBS +=	audio/libaudio.a math/libmath.a common/libcommon.a common/formats/libformats.a common/compression/libcompression.a common/libcommon.a image/libimage.a graphics/libgraphics.a

ifeq ($(ENABLE_WINTERMUTE), STATIC_PLUGIN)
	TESTS += $(srcdir)/test/engines/wintermute/*.h
	TEST_LIBS += engines/wintermute/libwintermute.a
endif

ifeq ($(ENABLE_ULTIMA), STATIC_PLUGIN)
ifdef ENABLE_ULTIMA8
	TESTS += $(srcdir)/test/engines/ultima/ultima8/*/*.h
endif
	TEST_LIBS += engines/ultima/libultima.a
endif

ifeq ($(ENABLE_TWINE), STATIC_PLUGIN)
	TESTS += $(srcdir)/test/engines/twine/*.h
	TEST_LIBS += engines/twine/libtwine.a
endif

ifeq ($(ENABLE_SCUMM), STATIC_PLUGIN)
	TESTS += $(srcdir)/test/engines/scumm/*.h
	TEST_LIBS += engines/scumm/libscumm.a
endif

ifeq ($(ENABLE_SWORD1), STATIC_PLUGIN)
	TESTS += $(srcdir)/test/engines/sword1/*.h
	TEST_LIBS += engines/sword1/libsword1.a
endif
ifeq ($(ENABLE_SWORD2), STATIC_PLUGIN)
	TESTS += $(srcdir)/test/engines/sword2/*.h
	TEST_LIBS += engines/sword2/libsword2.a
endif
ifeq ($(ENABLE_SKY), STATIC_PLUGIN)
	TESTS += $(srcdir)/test/engines/sky/*.h
	TEST_LIBS += engines/sky/libsky.a
endif
ifeq ($(ENABLE_GOB), STATIC_PLUGIN)
	TESTS += $(srcdir)/test/engines/gob/*.h
	TEST_LIBS += engines/gob/libgob.a
endif
ifeq ($(ENABLE_TINSEL), STATIC_PLUGIN)
	TESTS += $(srcdir)/test/engines/tinsel/*.h
	TEST_LIBS += engines/tinsel/libtinsel.a
endif
ifeq ($(ENABLE_AGS), STATIC_PLUGIN)
	TESTS += $(srcdir)/test/engines/ags/*.h
	TEST_LIBS += engines/ags/libags.a
endif
ifeq ($(ENABLE_SCI), STATIC_PLUGIN)
	TESTS += $(srcdir)/test/engines/sci/*.h
	TEST_LIBS += engines/sci/libsci.a
endif
ifeq ($(ENABLE_TOON), STATIC_PLUGIN)
	TESTS += $(srcdir)/test/engines/toon/*.h
	TEST_LIBS += engines/toon/libtoon.a
endif
ifeq ($(ENABLE_AGI), STATIC_PLUGIN)
	TESTS += $(srcdir)/test/engines/agi/*.h
	TEST_LIBS += engines/agi/libagi.a
endif
ifeq ($(ENABLE_KYRA), STATIC_PLUGIN)
	TESTS += $(srcdir)/test/engines/kyra/*.h
	TEST_LIBS += engines/kyra/libkyra.a
endif
ifeq ($(ENABLE_AGOS), STATIC_PLUGIN)
	TESTS += $(srcdir)/test/engines/agos/*.h
	TEST_LIBS += engines/agos/libagos.a
endif
ifeq ($(ENABLE_ASYLUM), STATIC_PLUGIN)
	TESTS += $(srcdir)/test/engines/asylum/*.h
	TEST_LIBS += engines/asylum/libasylum.a
endif

# The MCP naming/normalization helpers are shared by the per-engine MCP test
# suites. They are listed as bare objects rather than engines/libengines.a so
# that engine.o's GUI/base globals stay out of the cxxtest runner — the same
# reasoning that already applies to mcp_server.o. Listed once even when several
# engines are enabled, so the object is not passed to the linker twice.
# They land after the archives above, so — as with the libcommon/libformats
# cycle — the archives they draw on (JSON parsing, Common::String) have to be
# repeated afterwards for linkers that resolve archives strictly in order.
ifneq (,$(filter STATIC_PLUGIN,$(ENABLE_SCUMM) $(ENABLE_SWORD1) $(ENABLE_SWORD2) $(ENABLE_SKY) $(ENABLE_GOB) $(ENABLE_TINSEL) $(ENABLE_TOON) $(ENABLE_SCI) $(ENABLE_AGS)))
	TEST_LIBS += backends/networking/mcp/mcp_server.o engines/mcp_bridge_text.o \
		common/libcommon.a common/formats/libformats.a common/libcommon.a
endif

#
TEST_FLAGS   := --runner=StdioPrinter --no-std --no-eh
TEST_CFLAGS  := $(CFLAGS) -I$(srcdir)/test/cxxtest
TEST_LDFLAGS := $(LDFLAGS) $(LIBS)
TEST_CXXFLAGS  := $(filter-out -Wglobal-constructors,$(CXXFLAGS))
TEST_CXXFLAGS += -Wno-self-assign-overloaded

ifdef WIN32
TEST_LDFLAGS := $(filter-out -mwindows,$(TEST_LDFLAGS))
endif

ifdef N64
TEST_LDFLAGS := $(filter-out -mno-crt0,$(TEST_LDFLAGS))
endif

ifdef PSP
TEST_LIBS += backends/platform/psp/memory.o \
	backends/platform/psp/mp3.o \
	backends/platform/psp/trace.o
endif

# Enable this to get an X11 GUI for the error reporter.
#TEST_FLAGS   += --gui=X11Gui
#TEST_LDFLAGS += -L/usr/X11R6/lib -lX11


test: test/runner
	./test/runner
test/runner: test/runner.cpp $(TEST_LIBS) copy-dat
	+$(QUIET_CXX)$(LD) $(TEST_CXXFLAGS) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ test/runner.cpp $(TEST_LIBS) $(TEST_LDFLAGS)
test/runner.cpp: $(TESTS) $(srcdir)/test/module.mk
	@mkdir -p test
	$(srcdir)/test/cxxtest/bin/cxxtestgen $(TEST_FLAGS) -o $@ $+

clean: clean-test
clean-test:
	-$(RM) test/runner.cpp test/runner test/engine-data/encoding.dat test/system/null_osystem.o
	-rmdir test/engine-data

test/engine-data/encoding.dat: $(srcdir)/dists/engine-data/encoding.dat
	$(MKDIR) test/engine-data
	$(CP) $(srcdir)/dists/engine-data/encoding.dat test/engine-data/encoding.dat

copy-dat: test/engine-data/encoding.dat

.PHONY: test clean-test copy-dat
