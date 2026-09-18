#ifndef LCODE_TRUST_H
#define LCODE_TRUST_H

/*
 * Directory trust model: lcode only touches directories the user has
 * explicitly trusted. Trusted directories are persisted to a config file so
 * the user is only asked once per directory tree.
 */

const char *trust_config_path(void);

int trust_load(void);
void trust_free(void);

int trust_list(void);
int trust_ask(const char *dir);
int trust_add(const char *dir);
const char *trust_matching_root(const char *dir);

int trust_set_active(const char *dir);
const char *trust_active_root(void);

char *trust_canonical(const char *path);
int trust_path_allowed(const char *path);

#endif
