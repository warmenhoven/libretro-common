TARGET := retro_eventcount_test

LIBRETRO_COMM_DIR := ../../..

# Builds the real primitive, not a copy: retro_eventcount.c and
# rthreads.c straight out of the tree, retro_atomic.h header-only.  A
# change to any of the three is what this harness exists to catch.
SOURCES := \
	retro_eventcount_test.c \
	$(LIBRETRO_COMM_DIR)/rthreads/retro_eventcount.c \
	$(LIBRETRO_COMM_DIR)/rthreads/rthreads.c

CFLAGS  += -Wall -W -std=gnu99 -g -O1 \
           -DHAVE_THREADS -I$(LIBRETRO_COMM_DIR)/include
LDFLAGS += -lpthread
# rthreads.c uses clock_gettime + CLOCK_REALTIME on Linux glibc; on
# older glibc those live in -lrt.  Harmless on newer glibc.
LDFLAGS += -lrt

# SCOND=1 forces the condition-variable backend, which is what macOS,
# the BSDs and the console ports get.  'check' runs both: the default
# build covers this host's address-wait path, the SCOND build covers
# the path most targets actually take.
ifneq ($(SCOND),)
   CFLAGS += -DRETRO_EVENTCOUNT_FORCE_SCOND
   TARGET := retro_eventcount_test_scond
endif

OBJS := $(SOURCES:.c=.o)

ifneq ($(SAN),)
   SANITIZER := $(SAN)
endif
ifneq ($(SANITIZER),)
   CFLAGS  := -fsanitize=$(SANITIZER) -fno-omit-frame-pointer $(CFLAGS)
   LDFLAGS := -fsanitize=$(SANITIZER) $(LDFLAGS)
endif

all: $(TARGET)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

check: all
	./$(TARGET)
	@$(MAKE) -s clean
	@$(MAKE) -s SCOND=1 $(if $(SANITIZER),SANITIZER=$(SANITIZER))
	./retro_eventcount_test_scond

clean:
	rm -f $(OBJS) retro_eventcount_test retro_eventcount_test_scond

.PHONY: all check clean
