// flags: 
/* a‮b */
/* c‮d
   e⁦f */
// g‮h
char *s1 = "i‮j";
char *s2 = "k‮l⁦m";
char *s3 = "n‮p‬q";
char *s4 = "r‮t⁦u⁩v‬";
char *s5 = "w‬x";
char *s6 = "y‎z";
char *s7 = "\u202e";
int main(void) { return 0; }
