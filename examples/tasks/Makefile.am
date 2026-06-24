AUTOMAKE_OPTIONS = 1.6
AM_CPPFLAGS = -I $(top_srcdir)/include
if USE_INTERNAL_REGEX
AM_CPPFLAGS += -I$(top_srcdir)/include/internal/regex
endif
LDADD = $(top_builddir)/lib/libsword.la

noinst_PROGRAMS = findHeadings listExtantModuleBookNames parallelBibles simpleChapter simpleRange simpleVerseLookup

findHeadings_SOURCES = findHeadings.cpp
listExtantModuleBookNames_SOURCES = listExtantModuleBookNames.cpp
parallelBibles_SOURCES = parallelBibles.cpp
simpleChapter_SOURCES = simpleChapter.cpp
simpleRange_SOURCES = simpleRange.cpp
simpleVerseLookup_SOURCES = simpleVerseLookup.cpp
