// flags: -std=c99 -pedantic
#pragma pack(pop)
#pragma pack(push, a, 1)
#pragma pack(pop, c)
#pragma pack(pop)
#pragma pack(push, a, 3)
#pragma pack(1 2)
#pragma pack(2.5)
#pragma pack(bogus)
#pragma pack
#pragma pack(pop, a, 1)
#pragma pack(4) junk
#pragma pack(push, 8)
struct S { char c; long l; };
_Static_assert(sizeof(struct S) == 16, "pack 8");
#pragma pack(pop)
#pragma GCC diagnostic fatal "-Wunused"
#pragma GCC diagnostic
#pragma GCC diagnostic ignored
#pragma GCC diagnostic ignored "-Wnoexcept"
#pragma GCC diagnostic ignored "-fstrict-aliasing"
#pragma GCC diagnostic ignored "-Wunuesd"
#pragma GCC diagnostic ignored "-Wno-vla"
#pragma GCC diagnostic ignored "-Wunused"
#pragma redefine_extname foo
#pragma redefine_extname foo bar baz
#pragma scalar_storage_order
#pragma scalar_storage_order medium-endian
#pragma scalar_storage_order big-endian junk
