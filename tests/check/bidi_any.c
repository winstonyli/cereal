// flags: -Wbidi-chars=any
/* a‮b */
/* ‬ */
char *s1 = "i‮j‎k";
char *s2 = "⁦‮⁩";
char *s3 = "‮⁩";
char *s4 = "\u202e";
