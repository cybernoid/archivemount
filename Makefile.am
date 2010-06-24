bin_PROGRAMS = archivemount
archivemount_SOURCES = archivemount.c
archivemount_LDADD = $(ARCHIVE_LIBS) $(FUSE_LIBS)
archivemount_CFLAGS = $(FUSE_CFLAGS)
if DEBUG
archivemount_CFLAGS += -g
else
archivemount_CFLAGS += -O2
endif

dist_man_MANS = archivemount.1
EXTRA_DIST = CHANGELOG
