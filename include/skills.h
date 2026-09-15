#ifndef LCODE_SKILLS_H
#define LCODE_SKILLS_H

void skills_reload(void);
void skills_free(void);
char *skills_system_prompt(void);
char *skills_execute(const char *name);
int skills_list(void);

#endif
