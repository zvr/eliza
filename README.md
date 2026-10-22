# eliza

A C implementation of Joseph Weizenbaum's 1966 ELIZA program.

The program uses the original 1966 S-expression rules, also present in the repo.

## Build and run

Build with GCC in GNU C23 mode:

```sh
gcc -std=gnu23 -Wall -Wextra -Wpedantic -O2 eliza.c -o eliza
./eliza ./1966_CACM_article_Eliza_script
```

## Tests

You can run the built-in regression suite with:

```sh
./eliza --test
```

For memory and undefined-behavior checks, build and run the same suite with
sanitizers:

```sh
gcc -std=gnu23 -Wall -Wextra -Wpedantic -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    eliza.c -o eliza-test
ASAN_OPTIONS=detect_leaks=1 ./eliza-test --test
```

