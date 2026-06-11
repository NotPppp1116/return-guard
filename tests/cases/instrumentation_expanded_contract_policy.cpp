#include "include/expanded_system_api.hpp"

static void consume_status(int value) {
    (void)value;
}

static void consume_pointer(void* value) {
    (void)value;
}

void expanded_contract_policy_sample(void) {
    int pair[2] = {0, 0};
    char* stream_buffer = nullptr;
    unsigned long stream_size = 0;
    char link_buffer[64] = {};

    consume_pointer(fmemopen(nullptr, 0, "r"));
    consume_pointer(open_memstream(&stream_buffer, &stream_size));
    consume_pointer(fdopendir(3));
    consume_pointer(dlopen("libexample.so", 0));
    consume_pointer(wl_display_create());
    consume_pointer(wl_event_loop_add_fd(nullptr, 3, 1U, nullptr, nullptr));

    consume_status(remove("old.tmp"));
    consume_status(rename("old.tmp", "new.tmp"));
    consume_status(socketpair(1, 1, 0, pair));
    consume_status(getsockname(pair[0], nullptr, nullptr));
    consume_status(setsockopt(pair[0], 1, 2, nullptr, 0));
    consume_status(pipe2(pair, 0));
    consume_status(eventfd(0U, 0));
    consume_status(inotify_init1(0));
    consume_status(epoll_ctl(3, 1, pair[0], nullptr));
    consume_status(timerfd_settime(3, 0, nullptr, nullptr));
    consume_status(chdir("."));
    consume_status(mkdir("tmp", 0700U));
    consume_status(unlinkat(-1, "old.tmp", 0));
    consume_status(symlinkat("target", -1, "link"));
    consume_status(readlink("link", link_buffer, sizeof(link_buffer)));
    consume_status(fchmodat(-1, "tmp", 0700U, 0));
    consume_status(fchownat(-1, "tmp", 0U, 0U, 0));
    consume_status(faccessat(-1, "tmp", 0, 0));
    consume_status(fstatat(-1, "tmp", nullptr, 0));
    consume_status(ppoll(nullptr, 0, nullptr, nullptr));
    consume_status(pselect(0, nullptr, nullptr, nullptr, nullptr, nullptr));
    consume_status(wl_display_flush(nullptr));
    consume_status(wl_event_source_remove(nullptr));
    consume_status(wl_proxy_add_listener(nullptr, nullptr, nullptr));

    consume_status(vendor::chdir("."));
    consume_status(vendor::remove("tmp"));
    consume_pointer(vendor::wl_display_create());
}
