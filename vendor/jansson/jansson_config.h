/* jansson 2.14's jansson_config.h, filled in for the Vita (arm-vita-eabi, newlib). */
#ifndef JANSSON_CONFIG_H
#define JANSSON_CONFIG_H

#ifdef __cplusplus
#define JSON_INLINE inline
#else
#define JSON_INLINE inline
#endif

#define JSON_INTEGER_IS_LONG_LONG 1
#define JSON_HAVE_LOCALECONV 0
#define JSON_HAVE_ATOMIC_BUILTINS 1
#define JSON_HAVE_SYNC_BUILTINS 1
#define JSON_PARSER_MAX_DEPTH 2048

#endif
