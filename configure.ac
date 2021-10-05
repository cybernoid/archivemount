# Process this file with autoconf to produce a configure script

AC_INIT([archivemount],[0.9.1], [andrel@cybernoia.de])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_PROG_CC
AM_PROG_CC_C_O

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile archivemount.1])


RELEASE_DATE="20 April 2020"
AC_SUBST(RELEASE_DATE)

# Check for libfuse
PKG_CHECK_EXISTS(fuse)
PKG_CHECK_MODULES([FUSE], [fuse >= 2.6],
    AC_DEFINE([HAVE_FUSE2], [], [Need to use the fuse 2.6 API]),
    PKG_CHECK_MODULES([FUSE], [fuse3 >= 3],
        AC_DEFINE([HAVE_FUSE3], [], [Need to use the fuse 3.0 API]),
        AC_MSG_ERROR([libfuse>=2.6 not found.

                  If libfuse is installed then perhaps you should set
                  the PKG_CONFIG_PATH=/dir/containing/fuse.pc environment
                  variable]))
    )
AC_SUBST([FUSE_LIBS])
AC_SUBST([FUSE_CFLAGS])


# Check for libarchive (no pkg-config support)
AC_CHECK_LIB([archive], [archive_read_new], [ARCHIVE_LIBS=-larchive],
    AC_MSG_ERROR([libarchive not found.
                  If libarchive is installed then perhaps you should set
                  the LDFLAGS=-L/nonstandard/lib/dir environment variable]))
AC_SUBST([ARCHIVE_LIBS])
AC_CHECK_HEADERS([archive.h archive_entry.h],,
    AC_MSG_ERROR([libarchive headers not found.

                  If the libarchive headers are installed then perhaps you
                  should set the CPPFLAGS=-I/nonstandard/include/dir
                  environment variable]))

AC_CHECK_HEADERS([regex.h],,
    AC_MSG_ERROR([regex.h not found.

                  If the regex.h header file is installed then perhaps you
                  should set the CPPFLAGS=-I/nonstandard/include/dir
                  environment variable]))


# Debug support
AC_MSG_CHECKING(if you want debug support)
debug_on=no
AC_ARG_ENABLE(debug, AC_HELP_STRING([--enable-debug],
                                    [enable debug support (def=no)]),
              debug_on="$enableval")
AC_MSG_RESULT($debug_on)
if test "$debug_on" = "no"; then
        AC_DEFINE([NDEBUG],, [Define to drop debug support])
fi
AM_CONDITIONAL(DEBUG, test "$debug_on" = yes)

AC_OUTPUT

