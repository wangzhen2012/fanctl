.PHONY: all check build clean

CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra
FRAMEWORKS = -framework IOKit -framework CoreFoundation

all: build

build: fanctl-smc

fanctl-smc: fanctl-smc.c
	$(CC) $(CFLAGS) -o $@ $< $(FRAMEWORKS)

check:
	$(CC) -fsyntax-only fanctl-smc.c $(FRAMEWORKS)
	python3 -m py_compile fanctl_menubar.py mini_test.py

clean:
	rm -f fanctl-smc debug-smc
	rm -rf __pycache__
