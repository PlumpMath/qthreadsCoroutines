dnl -*- Autoconf -*-
dnl
dnl Copyright (c)      2010  Sandia Corporation
dnl
dnl
dnl QTHREAD_CHECK_BITFIELDS
dnl ------------------------------------------------------------------------------
AC_DEFUN([QTHREAD_CHECK_BITFIELDS], [
AC_CACHE_CHECK([bitfield ordering],
    [qthread_cv_bitfield_order],
    [AC_RUN_IFELSE(
	   [AC_LANG_PROGRAM([[
#include <assert.h>
union foo {
	unsigned long w;
	struct bar {
		unsigned long a : 60;
		unsigned b : 3;
		unsigned c : 1;
	} s;
} fb;]],
[[
fb.w = 0;
fb.s.c = 1;
assert(fb.w == 1);]])],
	[qthread_cv_bitfield_order="forward"],
	[qthread_cv_bitfield_order="reverse"],
	[qthread_cv_ucstack_ssflags="assuming reverse"])])
AS_IF([test "$qthread_cv_bitfield_order" == forward],
	  [AC_DEFINE([BITFIELD_ORDER_FORWARD], [1], [Define if bitfields are in forward order])],
	  [AC_DEFINE([BITFIELD_ORDER_REVERSE], [1], [Define if bitfields are in reverse order])])
])