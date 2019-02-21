EXTENSION = pg_plan_advsr
DATA = pg_plan_advsr--0.0.sql 

OBJS = pg_plan_advsr.o pgsp_json.o pgsp_json_text.o

MODULE_big = $(EXTENSION)

PG_CPPFLAGS = -I$(libpq_srcdir)
SHLIB_LINK = $(libpq)

ifdef NO_PGXS
subdir = contrib/pg_plan_advsr
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
else
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
endif

