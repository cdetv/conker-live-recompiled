/* The Milk Fix's glass of milk (Extras, to-do 45; milk_glass.c). */
#ifndef CONKER_MILK_GLASS_H
#define CONKER_MILK_GLASS_H

#include <stddef.h>
#include <stdint.h>

/* Where the glass of milk goes in the asset cache (below assets\), and the
 * goblet's model name it stands in for once one letter changes ("milk" ->
 * "mil2"; the letter is at 0x005EDF99 in this XBE). */
#define MILK_GLASS_OUT "aid\\bfdmodel\\attach\\effects_models_conker_mil2\\default.rbm"

/* The front end's whiskey glass (its carved bytes, changed in place) made a
 * glass of milk: 1, or 0 when the file is not the glass this was made for. */
int milk_glass_make(uint8_t *file, size_t size);

#endif
