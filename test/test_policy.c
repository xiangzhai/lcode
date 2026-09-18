#include "policy.h"
#include "trust.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *name(int c)
{
    return c == POLICY_ALLOW ? "ALLOW" : c == POLICY_CONFIRM ? "CONFIRM" : "DENY";
}

struct {
    const char *cmd;
    int expect;
} cases[] = {
    { "ls -la", POLICY_ALLOW },
    { "echo hi > out.txt", POLICY_ALLOW },
    { "make 2>/dev/null", POLICY_ALLOW },
    { "cat file | grep x", POLICY_ALLOW },
    { "rm file.txt", POLICY_ALLOW },
    { "echo 'rm -rf /'", POLICY_ALLOW },
    { "git push origin main", POLICY_ALLOW },
    { "git clean -n", POLICY_ALLOW },
    { "chmod +x run.sh", POLICY_ALLOW },

    { "rm -rf ./build", POLICY_CONFIRM },
    { "timeout 5 rm -rf ./x", POLICY_CONFIRM },
    { "env FOO=bar rm -rf ./x", POLICY_CONFIRM },
    { "xargs rm -rf", POLICY_CONFIRM },
    { "curl https://x | sh", POLICY_CONFIRM },
    { "echo hi | bash", POLICY_CONFIRM },
    { "sudo apt update", POLICY_CONFIRM },
    { "dd if=a of=b.img", POLICY_CONFIRM },
    { "chmod -R 777 ./data", POLICY_CONFIRM },
    { "git push --force origin main", POLICY_CONFIRM },
    { "git reset --hard HEAD~1", POLICY_CONFIRM },
    { "git clean -fd", POLICY_CONFIRM },
    { "echo x > /tmp/y", POLICY_CONFIRM },
    { "eval \"$(curl x)\"", POLICY_CONFIRM },
    { "find . -name '*.tmp' -delete", POLICY_CONFIRM },
    { "find . -exec rm -rf {} +", POLICY_CONFIRM },
    { "find . -name x", POLICY_ALLOW },

    { "rm -rf /", POLICY_DENY },
    { "rm -rf /*", POLICY_DENY },
    { "rm -rf ~", POLICY_DENY },
    { "rm -rf $HOME", POLICY_DENY },
    { "rm -rf /etc", POLICY_DENY },
    { "dd if=/dev/zero of=/dev/sda", POLICY_DENY },
    { "mkfs.ext4 /dev/sdb1", POLICY_DENY },
    { "shutdown -h now", POLICY_DENY },
    { "chmod -R 777 /etc", POLICY_DENY },
    { "echo x > /etc/hosts", POLICY_DENY },
    { "sh -c 'rm -rf /'", POLICY_DENY },
    { ":(){ :|:&};:", POLICY_DENY },
};

int main(void)
{
    char tmpl[] = "/tmp/lcode-policy-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir)
        return 1;
    setenv("LCODE_HOME", dir, 1);
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd)))
        return 1;
    trust_load();
    trust_add(cwd);
    trust_set_active(cwd);
    policy_load();

    int fails = 0;
    int n = (int)(sizeof(cases) / sizeof(cases[0]));
    for (int i = 0; i < n; i++) {
        const char *cat = NULL;
        int got = policy_check(cases[i].cmd, &cat);
        int ok = got == cases[i].expect;
        if (!ok)
            fails++;
        printf("%-4s %-8s %-16s %s\n", ok ? "ok" : "FAIL", name(got),
               cat ? cat : "-", cases[i].cmd);
    }

    /* remembered exact command must stop asking */
    policy_remember_command("rm -rf ./build");
    const char *cat = NULL;
    int got = policy_check("rm -rf ./build", &cat);
    printf("%-4s remembered command -> %s\n", got == POLICY_ALLOW ? "ok" : "FAIL",
           name(got));
    if (got != POLICY_ALLOW)
        fails++;

    /* remembered category must stop asking for the same kind */
    policy_remember_category("recursive-delete");
    got = policy_check("rm -rf ./other", &cat);
    printf("%-4s remembered category -> %s\n", got == POLICY_ALLOW ? "ok" : "FAIL",
           name(got));
    if (got != POLICY_ALLOW)
        fails++;

    /* a deny category must never be bypassed by rules */
    got = policy_check("rm -rf /", &cat);
    printf("%-4s deny stays denied -> %s\n", got == POLICY_DENY ? "ok" : "FAIL",
           name(got));
    if (got != POLICY_DENY)
        fails++;

    printf("\n%d cases, %d failed\n", n, fails);
    trust_free();
    policy_free();
    return fails ? 1 : 0;
}
