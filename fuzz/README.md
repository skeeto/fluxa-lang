# fuzz/ — libFuzzer harnesses (ASan + UBSan)

Six parser harnesses, one binary each, built with `clang -fsanitize=fuzzer,address,undefined`.

| Target  | Entry point                          | What it exercises                                                                                                                     |
|---------|--------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------|
| lexer   | `lexer_next` until EOF/error         | escape handling, identifier/number cutoffs, OOB past `l->len`                                                                         |
| parser  | `parser_parse` over a fresh ASTPool  | every grammar production, plus pool overflow paths (calloc/strdup fallback)                                                           |
| toml    | `fluxa_config_load` + `_load_libs`   | `cfg_trim` / `cfg_unquote` / `cfg_parse_sig`, all section dispatch                                                                    |
| csv     | `csv.field` / `csv.field_count`      | quoted fields, `""` escapes, unterminated quotes, custom delimiters                                                                   |
| json    | `json_read_string` + `json_find_key` | `\`-escape walker (incl. EOF after `\`), flat-object key skip with brace/bracket counting                                             |
| json2   | `j2_parse` + `j2_navigate` + stringify | recursive descent with calloc'd nodes, 4 KiB-bounded strings, path navigator over `a.b.c[i]`                                          |

## Running

```bash
make fuzz-build          # compile all harnesses → fuzz/build/fuzz_*
make fuzz-parser         # build + run parser harness for FUZZ_TIME=60s
make fuzz                # build + run every target sequentially
make fuzz FUZZ_TIME=600  # 10 minutes per target
make fuzz-clean          # wipe fuzz/build and fuzz/findings
```

Findings (crash inputs, leak inputs, timeouts) land in `fuzz/findings/<target>/`. Reproduce with:

```bash
./fuzz/build/fuzz_<target> fuzz/findings/<target>/crash-<hash>
```

## Adding a target

1. Drop `fuzz/fuzz_<name>.c` defining `LLVMFuzzerTestOneInput`. Use `fuzz_common.h` for the `(data, size) → null-terminated char*` copy that lets ASan catch reads past the logical end.
2. Add `FUZZ_<NAME>_SRCS` (any extra `.c` files to link in), append `<name>` to `FUZZ_TARGETS`, and add the build rule in the Makefile.
3. Drop seeds into `fuzz/corpus/<name>/`.

## Notes

- `_POSIX_C_SOURCE=200809L` is set so `strdup` / `strtok_r` resolve.
- `-DFLUXA_HAS_FFI=0` disables FFI in `scope.h` (we don't link libffi).
- Each harness includes only the source needed to drive its parser — no runtime, no IPC, no stdlib bookkeeping. That keeps iterations fast and reduces sanitizer noise from unrelated code.
