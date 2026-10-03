// flags: -Wduplicated-cond
int ff1(int i,int j){
  if (i<j) return 1; else if (j>i) return 2; /*P0*/
  if (i<=j) return 1; else if (j>=i) return 2; /*P1*/
  if (i==j) return 1; else if (j==i) return 2; /*P2*/
  if (i+1==3) return 1; else if (i==2) return 2; /*P3*/
  if (i-1==3) return 1; else if (i==4) return 2; /*P4*/
  if (1+i==3) return 1; else if (i==2) return 2; /*P5*/
  if (!(i<j)) return 1; else if (i>=j) return 2; /*P6*/
  if (!(i==j)) return 1; else if (i!=j) return 2; /*P7*/
  if (i) return 1; else if (i!=0) return 2; /*P8*/
  if (!i) return 1; else if (i==0) return 2; /*P9*/
  if (i<j) return 1; else if (!(i>=j)) return 2; /*P10*/
  if (i&&j) return 1; else if (j&&i) return 2; /*P11*/
  if (i<2) return 1; else if (i<=1) return 2; /*P12*/
  if (i+j) return 1; else if (j+i) return 2; /*P13*/
  if ((i)) return 1; else if (i) return 2; /*P14*/
  if (i*2) return 1; else if (i<<1) return 2; /*P15*/
  if (i>0) return 1; else if (0<i) return 2; /*P16*/
  if (i==0) return 1; else if (!i) return 2; /*P17*/
  if ((long)i) return 1; else if ((long)i) return 2; /*P18*/
  if (i>j) return 1; else if (j<i) return 2; /*P19*/
  if (i+1<3) return 1; else if (i<2) return 2; /*P20*/
  if (-i<3) return 1; else if (i>-3) return 2; /*P21*/
  if (i<j) return 1; else if (i>j) return 2; /*P22*/
  return 0;
}
int ff2(int i,int j,unsigned u,float f,float g,int *p,char c,long l){
  if (u+1<3) return 1; else if (u<2) return 2;
  if (u+1==3) return 1; else if (u==2) return 2;
  if (!(f<g)) return 1; else if (f>=g) return 2;
  if (f<g) return 1; else if (g>f) return 2;
  if (p) return 1; else if (p!=0) return 2;
  if (!p) return 1; else if (p==0) return 2;
  if (u-1u<3) return 1; else if (u<4) return 2;
  if (-u<3) return 1; else if (u>-3) return 2;
  if (i<2) return 1; else if (i<=1) return 2;
  if (i>=2) return 1; else if (i>1) return 2;
  if (i<=2) return 1; else if (i<3) return 2;
  if (c+1==3) return 1; else if (c==2) return 2;
  if (!!i) return 1; else if (i) return 2;
  if (!!i) return 1; else if (i!=0) return 2;
  if (i==j) return 1; else if (!(i!=j)) return 2;
  if (i-1<2) return 1; else if (i<3) return 2;
  if (2<i) return 1; else if (i>2) return 2;
  if (l+1<3) return 1; else if (l<2) return 2;
  return 0;
}
int ff3(int i,int j){
  if (i+j) return 1; else if (i+j) return 2;
  if (i+j) return 1; else if (j+i) return 2;
  if (i*j) return 1; else if (j*i) return 2;
  if (i&j) return 1; else if (j&i) return 2;
  if (i==j) return 1; else if (j==i) return 2;
  if (i<j) return 1; else if (j>i) return 2;
  if (i-j) return 1; else if (i-j) return 2;
  return 0;
}
