# Copyright (C) 2023 Alexios Zavras
# Copyright (C) 2023 Fotios Valasiadis
# SPDX-License-Identifier: LGPL-2.1-or-later

bin_PROGRAMS = $(top_builddir)/build-recorder

__top_builddir__build_recorder_SOURCES = \
	hash.c \
	main.c \
	schema.c \
	record.c \
	tracer.c \
	$(EMPTY)

dist_noinst_HEADERS = *.h

EXTRA_DIST = schema.c

SCHEMA = ../doc/build-recorder-schema.ttl

dist_data_DATA = $(SCHEMA)

if HAVE_XXD
schema.c: $(SCHEMA)
	sed '/^#/d' $^ | $(XXD) -i -n schema > $@
endif
