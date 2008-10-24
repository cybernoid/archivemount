bin_PROGRAMS = archivemount
archivemount_SOURCES = archivemount.c
archivemount_LDADD = $(ARCHIVE_LIBS) $(FUSE_LIBS)
archivemount_CFLAGS = $(FUSE_CFLAGS)
if DEBUG
archivemount_CFLAGS += -g
endif

EXTRA_DIST = CHANGELOG
