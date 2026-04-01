CC = cc
CFLAGS = -std=c99 -Wall -Wextra -O3 -flto -D_POSIX_C_SOURCE=200809L
slap: slap.c
	$(CC) $(CFLAGS) -o slap slap.c -lm
slap-sdl: slap.c
	$(CC) $(CFLAGS) -DSLAP_SDL -o slap-sdl slap.c -lm $(shell sdl2-config --cflags --libs 2>/dev/null || echo "-lSDL2")
clean:
	rm -f slap slap-sdl
test: slap
	@./slap tests/expect.slap
	@./slap --check tests/type.slap
	@./slap tests/type.slap > /dev/null
	@./slap --check tests/expect.slap
	@python3 tests/run_panic.py
	@python3 tests/run_type_errors.py
	@echo "All test suites passed."
.PHONY: clean test
