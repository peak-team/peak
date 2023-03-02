# Top-level Makefile

# List of subdirectories
SUBDIRS := lib_prof peak_malloc perf_counter

# Targets that will be built by default
all: $(SUBDIRS)

# Build all subdirectories
$(SUBDIRS):
	$(MAKE) -C $@

# Clean all subdirectories

.PHONY: clean
.PHONY: veryclean
clean:
	for dir in $(SUBDIRS); do \
              $(MAKE) -C $$dir clean; \
	done
veryclean:
	for dir in $(SUBDIRS); do \
              $(MAKE) -C $$dir veryclean; \
	done


.PHONY: all $(SUBDIRS)

