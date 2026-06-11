#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <returnguard/Runtime.h>

static unsigned char registered_secret[] = {1U, 2U, 3U, 4U};
static unsigned char late_region[4];

void __rg_fatal_hook(uint64_t site_id, int saved_errno) {
    (void)site_id;
    (void)saved_errno;

    const int secret_was_wiped =
        registered_secret[0] == 0U && registered_secret[1] == 0U &&
        registered_secret[2] == 0U && registered_secret[3] == 0U;
    const ReturnGuardSecretResult late_registration =
        returnguard_register_secret(late_region, sizeof(late_region));
    const ReturnGuardSecretResult late_unregistration =
        returnguard_unregister_secret(registered_secret);

    if (secret_was_wiped != 0 &&
        late_registration == RETURNGUARD_SECRET_BUSY &&
        late_unregistration == RETURNGUARD_SECRET_BUSY) {
        static const char success[] = "registry-closed\n";
        (void)write(2, success, sizeof(success) - 1U);
    } else {
        static const char failure[] = "registry-open\n";
        (void)write(2, failure, sizeof(failure) - 1U);
    }
}

int main(void) {
    if (returnguard_register_secret(
            registered_secret, sizeof(registered_secret)) !=
        RETURNGUARD_SECRET_OK) {
        return 1;
    }

    __rg_fatal(42U, 0);
}
