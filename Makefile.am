plugin_LTLIBRARIES = libgstvspmfilter.la

libgstvspmfilter_la_SOURCES =  gstvspmfilter.c

libgstvspmfilter_la_CFLAGS = \
	$(GST_PLUGINS_BASE_CFLAGS) \
	$(GST_VIDEO_CFLAGS) \
	$(GST_ALLOCATORS_CFLAGS) \
	$(GST_CFLAGS)
libgstvspmfilter_la_LIBADD = \
	$(GST_VIDEO_LIBS) \
	$(GST_ALLOCATORS_LIBS) \
	$(GST_BASE_LIBS) \
	$(GST_LIBS) \
	-lvspm \
	-lmmngr \
	-lmmngrbuf
libgstvspmfilter_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstvspmfilter_la_LIBTOOLFLAGS = $(GST_PLUGIN_LIBTOOLFLAGS)

noinst_HEADERS = gstvspmfilter.h
