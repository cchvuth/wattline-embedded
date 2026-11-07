CC ?= cc
HOSTFLAGS = -std=c11 -Wall -Wextra -fsanitize=address,undefined

test:
	python3 -I tools/gen_signals.py ../tesla-dash build
	$(CC) $(HOSTFLAGS) -Ibuild -o build/test_decode build/test_decode.c -lm && build/test_decode
	$(CC) $(HOSTFLAGS) -o build/test_panda test/test_panda.c core/panda.c && build/test_panda
	cd test && $(CC) $(HOSTFLAGS) -DAES256=1 -I../third_party -o ../build/test_sec1 sec1_crypto_check.c ../third_party/aes.c ../third_party/monocypher.c && ../build/test_sec1

.PHONY: test
