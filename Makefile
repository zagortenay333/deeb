.SILENT:

CC           := gcc
CC2          := gcc # For the -MM flag. See below.
CFLAGS       := -std=c11 -pedantic -Wall -Wextra -Werror=vla \
                -Wno-unused-function -Wno-missing-braces \
                -Wno-unused-value -Wno-unused-parameter \
                -D_POSIX_C_SOURCE=200809L
LDFLAGS      := -lreadline
src_dir      := src
prog_name    := shell
coverage_dir := tests/coverage
src_files    := $(wildcard $(src_dir)/*.c)
obj_files    := $(src_files:.c=.o)
dep_files    := $(src_files:.c=.dep)

# We want to ensure that an .o file is recompiled whenever it's corresponding
# .c file gets changed or any header file included by that .c file. In order
# to accomplish that we use the compiler flags -MM and -MT (not supported by
# every compiler) to produce lots of small makefiles (.dep) each containing
# a single rule that makes an .o file depend on the .c file and all included
# headers. The "-include" directive will both make the .dep files initially
# (using the "%.dep" rule) as well as include them here.
-include $(dep_files)
%.dep: %.c
	$(CC2) $(CFLAGS) $< -MM -MT $(@:.dep=.o) >$@

$(prog_name): $(obj_files)
	@$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

release: CFLAGS += -g -DRELEASE_BUILD -DNDEBUG -O2 -Wno-return-type -Wno-unused-variable
release: $(prog_name)

debug: CFLAGS += -DDEBUG_BUILD -g3 -fno-omit-frame-pointer
debug: $(prog_name)

asan: CFLAGS += -DASAN_BUILD -g3 -fno-omit-frame-pointer -fsanitize=address,undefined
asan: $(prog_name)

test: CFLAGS += -coverage
test: clean asan
	./$(prog_name) -i tests/scratch.sql -d tests/dummy.db
	rm -rf $(coverage_dir)
	mkdir $(coverage_dir)
	lcov --quiet --capture --directory $(src_dir) --output-file $(coverage_dir)/info
	genhtml --quiet -o $(coverage_dir) $(coverage_dir)/info

lines:
	find $(src_dir) -iname "*.c" -o -iname "*.h" | xargs wc -l | sort -g -r

clean:
	rm -rf $(src_dir)/*.gcno $(src_dir)/*.gcda $(prog_name) $(dep_files) $(obj_files) $(coverage_dir)

.PHONY := release debug asan test lines clean
