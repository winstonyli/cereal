/* Duplicate members among thousands (the check is linear in the member
 * count), through an anonymous member, and per record: a later record, a
 * record defined among another's members, may reuse every name. */
#define D(p) int p##0, p##1, p##2, p##3, p##4, p##5, p##6, p##7, p##8, p##9;
#define C(p) D(p##0) D(p##1) D(p##2) D(p##3) D(p##4) D(p##5) D(p##6) D(p##7) D(p##8) D(p##9)
#define M(p) C(p##0) C(p##1) C(p##2) C(p##3) C(p##4) C(p##5) C(p##6) C(p##7) C(p##8) C(p##9)
#define K(p) M(p##0) M(p##1) M(p##2) M(p##3)
struct Big {
  K(f)
  int f1234;
  struct { int g; int f3999; };
  struct Inner { K(f) int g; } in;
  int g;
};
struct Again { K(f) int g; };
