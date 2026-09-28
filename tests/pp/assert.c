/* GCC assertions (deprecated extension), including the host's predefined
 * ones. */
#if #system(linux) && #cpu(x86_64) && #machine(x86_64) && #system
host
#endif
#assert shape(round  thing)
#if #shape(round thing) && !#shape(square) && #shape
asserted
#endif
#unassert shape(round thing)
#if #shape
still_asserted
#endif
#assert color(red)
#assert color(blue)
#unassert color
#if !#color
all_gone
#endif
