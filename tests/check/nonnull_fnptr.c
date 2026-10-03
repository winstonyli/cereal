// flags: -Wnonnull
extern __attribute__ ((nonnull)) void fnonnull3 (void*);

void fnonnull_global_local (void)
{
  fnonnull3 (0);    // { dg-warning "\\\[-Wnonnull" }
}

void gnonnull_global_local (void)
{
  extern void fnonnull3 (void*);

  fnonnull3 (0);    // { dg-warning "\\\[-Wnonnull" }
}


void pfnonnull_local_local (void)
{
  extern __attribute__ ((nonnull)) void (*pfnonnull1) (void*);

  pfnonnull1 (0);   // { dg-warning "\\\[-Wnonnull" }
}

void gpnonnull_local_local (void)
{
  extern void (*pfnonnull1) (void*);

  pfnonnull1 (0);   // { dg-warning "\\\[-Wnonnull" "pr?????" { xfail *-*-* } }
}


void pfnonnull_local_global (void)
{
  extern __attribute__ ((nonnull)) void (*pfnonnull2) (void*);

  pfnonnull2 (0);   // { dg-warning "\\\[-Wnonnull" }
}

extern void (*pfnonnull2) (void*);

void gpnonnull_local_global (void)
{
  pfnonnull2 (0);   // { dg-warning "\\\[-Wnonnull" "pr?????" { xfail *-*-* } }
}


extern __attribute__ ((nonnull)) void (*pfnonnull3) (void*);

void pfnonnull_global_local (void)
{
  pfnonnull3 (0);   // { dg-warning "\\\[-Wnonnull" }
}

void gpnonnull_global_local (void)
{
  extern void (*pfnonnull3) (void*);

  pfnonnull3 (0);   // { dg-warning "\\\[-Wnonnull" }
}
