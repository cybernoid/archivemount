# Process this file with autoconf to produce a configure script

AC_INIT([archivemount],[0.6.0], [andrel@cybernoia.de] ) 
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_PROG_CC 

AC_CONFIG_HEADERS([config.h]) 
AC_CONFIG_FILES([Makefile]) 


PKG_CHECK_EXISTS(fuse)
PKG_CHECK_MODULES([FUSE], [fuse >= 2.6],,
    AC_MSG_ERROR([libfuse>=2.6 not found.  

                  If libfuse is installed then perhaps you should set
                  the PKG_CONFIG_PATH=/dir/containing/fuse.pc environment 
                  variable]))
AC_SUBST([FUSE_LIBS])
AC_SUBST([FUSE_CFLAGS])


# No pkg-config support from libarchive
AC_CHECK_LIB([archive], [archive_read_new], [ARCHIVE_LIBS=-larchive],
    AC_MSG_ERROR([libarchive not found.  
                  If libarchive is installed then perhaps you should set
                  the LDFLAGS=-L/nonstandard/lib/dir environment variable]))
AC_SUBST([ARCHIVE_LIBS])
AC_CHECK_HEADERS([archive.h archive_entry.h],,
    AC_MSG_ERROR([libarchive headers not found.  

                  If the libarchive headers are installed then perhaps you
                  should set the CPPFLAGS=-I/nonstandard/header/dir 
                  environment variable]))

AC_OUTPUT
