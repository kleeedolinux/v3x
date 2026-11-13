/* gfx_driver: PE import-table lookup (pure, works on any mapped image) + device-flag policy.
 * test/gfx_driver_test.c compiles this directly. */
#ifndef GD_IAT_H
#define GD_IAT_H
#include <stdint.h>

/* Address of the IAT slot for dll!func in a mapped PE image (case-insensitive dll name), or NULL. */
void **gd_find_iat_slot(uint8_t *base, const char *dll, const char *func);

#define GD_PREVENT_INTERNAL_THREADING 0x8u   /* D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS */
#define GD_SINGLETHREADED             0x1u   /* never added: the engine uses the context from several threads */

/* Flags to pass to D3D11CreateDevice given the engine's flags and the config. */
unsigned gd_device_flags(unsigned engine_flags, int prevent_threading);

#endif
