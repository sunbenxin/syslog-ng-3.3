export top_srcdir
AM_CPPFLAGS = -I$(top_srcdir)/lib -I../lib

if ENABLE_ENV_WRAPPER
libexec_PROGRAMS = syslog-ng
sbin_PROGRAMS = syslog-ng-wrapper syslog-ng-ctl
else
sbin_PROGRAMS = syslog-ng syslog-ng-ctl
endif

syslog_ng_SOURCES = main.c
syslog_ng_LDADD = -L../lib/.libs -lsyslog-ng @DEPS_LIBS@
syslog_ng_LINK =  @SYSLOGNG_LINK@
syslog_ng_wrapper_SOURCES = wrapper.c

syslog_ng_ctl_SOURCES = syslog-ng-ctl.c
syslog_ng_ctl_LDADD = ../lib/libsyslog-ng.la @DEPS_LIBS@

if ENABLE_ENV_WRAPPER

install-exec-hook:
	mv $(DESTDIR)/$(sbindir)/syslog-ng-wrapper $(DESTDIR)/$(sbindir)/syslog-ng
else

install-exec-hook:
	echo Nothing to do!
endif
