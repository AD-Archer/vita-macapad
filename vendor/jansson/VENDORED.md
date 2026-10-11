# jansson

[jansson](https://github.com/akheron/jansson) 2.14's `src/`, unchanged, MIT license (see `LICENSE`).

It's built with the app instead of linked from VitaSDK's package because that `libjansson.a` is
compiled as position-independent code, and `vita-elf-create` refuses its GOT relocations
("Invalid relocation type 25"). `jansson_config.h` and `jansson_private_config.h` stand in for
jansson's configure step.
