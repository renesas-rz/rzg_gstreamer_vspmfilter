plugin_LTLIBRARIES = libgstvspmfilter.la

libgstvspmfilter_la_SOURCES = gstvspmfilter.c
libgstvspmfilter_la_CFLAGS = \
	$(GST_PLUGINS_BASE_CFLAGS) \
	$(GST_CFLAGS)
libgstvspmfilter_la_LIBADD = \
	$(top_builddir)/gst-libs/gst/video/libgstvideo-$(GST_API_VERSION).la \
	$(top_builddir)/gst-libs/gst/allocators/libgstallocators-$(GST_API_VERSION).la \
	$(GST_BASE_LIBS) \
	$(GST_LIBS) \
	-lvspm \
	-lmmngr \
	-lmmngrbuf
libgstvspmfilter_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstvspmfilter_la_LIBTOOLFLAGS = $(GST_PLUGIN_LIBTOOLFLAGS)

noinst_HEADERS = gstvspmfilter.h

Android.mk: Makefile.am $(BUILT_SOURCES)
	androgenizer \
	-:PROJECT libgstvspmfilter -:SHARED libgstvspmfilter \
	 -:TAGS eng debug \
         -:REL_TOP $(top_srcdir) -:ABS_TOP $(abs_top_srcdir) \
	 -:SOURCES $(libgstvspmfilter_la_SOURCES) \
	           $(nodist_libgstvspmfilter_la_SOURCES) \
	 -:CFLAGS $(DEFS) $(DEFAULT_INCLUDES) $(libgstvspmfilter_la_CFLAGS) \
	 -:LDFLAGS $(libgstvspmfilter_la_LDFLAGS) \
	           $(libgstvspmfilter_la_LIBADD) \
	           -ldl \
	 -:PASSTHROUGH LOCAL_ARM_MODE:=arm \
		       LOCAL_MODULE_PATH:='$$(TARGET_OUT)/lib/gstreamer-0.10' \
	> $@
