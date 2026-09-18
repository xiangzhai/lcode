#ifndef LCODE_POLICY_H
#define LCODE_POLICY_H

/*
 * Shell command policy for run_shell. Directory trust decides *where* lcode may
 * work; this decides *what* a shell command may do. Every command is classified
 * as safe (run it), risky (confirm first) or forbidden (never run), so trusting
 * a directory no longer means letting destructive commands through unchecked.
 * A confirmation can be remembered so an approved command, or a whole class of
 * risky commands, is not asked about again.
 */

enum {
    POLICY_ALLOW = 0,   /* run it */
    POLICY_CONFIRM = 1, /* ask the user before running */
    POLICY_DENY = 2,    /* refuse to run, whatever the user says */
};

const char *policy_config_path(void);

int policy_load(void);
void policy_free(void);
int policy_list(void);
int policy_reset(void);

void policy_set_allow_all(int enabled);

/*
 * Classify a command. When the result is not POLICY_ALLOW and category is not
 * NULL, *category receives a static category key (see policy_category_desc)
 * that the caller can show, and that policy_remember_category() accepts.
 * A command the user already approved for this category, or by exact match,
 * classifies as POLICY_ALLOW.
 */
int policy_check(const char *command, const char **category);

/* Human-readable description for a category key, or NULL if unknown. */
const char *policy_category_desc(const char *category);

/* Remember an approved command / category so it is not asked about again. */
int policy_remember_command(const char *command);
int policy_remember_category(const char *category);

#endif
