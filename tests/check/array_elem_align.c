typedef struct {
  char c[129];
} B __attribute__((aligned(128)));

B a[2];

struct F {
  B b[4];
};

typedef B D[3];

void g(int n) {
  B v[n];
  B w[3];
}

typedef char C __attribute__((aligned(8)));

C x[3];
C y[8];
C *z;
